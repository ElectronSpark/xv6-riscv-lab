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

#define VIRTIO_GPU_F_VIRGL          0
#define VIRTIO_GPU_F_EDID           1
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
#define VIRTIO_GPU_CMD_CTX_CREATE         0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY        0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D  0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D 0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D          0x0207
#define VIRTIO_GPU_CMD_UPDATE_CURSOR      0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR        0x0301

/* virtio-gpu cursor resources are a fixed 64x64 BGRA image. */
#define VIRTIO_GPU_CURSOR_DIM   64
#define VIRTIO_GPU_CURSOR_RING  64
#define VIRTIO_GPU_RESP_OK_NODATA       0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO  0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET       0x1103
#define VIRTIO_GPU_RESP_OK_EDID         0x1104

#define VIRTIO_GPU_FLAG_FENCE  (1 << 0)
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_CAPSET_VIRGL          1
#define VIRTIO_GPU_CAPSET_VIRGL2         2
#define VIRTIO_GPU_CAPSET_DRM            6
#define VIRTIO_GPU_SMOKE_WIDTH 32
#define VIRTIO_GPU_SMOKE_HEIGHT 32
#define VIRTIO_GPU_MAX_RESOURCES 16384
#define VIRTIO_GPU_MAX_CONTEXTS 256
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
    uint32 target;
    uint32 bind;
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
    uint32 id;
    uint32 version;
    uint32 size;
};

struct virtio_gpu_drm_capset {
    uint32 wire_format_version;
    uint32 version_major;
    uint32 version_minor;
    uint32 version_patchlevel;
    uint32 context_type;
    uint32 pad;
    union {
        struct {
            uint32 has_cached_coherent;
            uint32 priorities;
            uint64 va_start;
            uint64 va_size;
            uint32 gpu_id;
            uint32 gmem_size;
            uint64 gmem_base;
            uint64 chip_id;
            uint32 max_freq;
            uint32 highest_bank_bit;
            uint64 ubwc_swizzle;
            uint64 macrotile_mode;
            uint32 has_raytracing;
            uint32 has_preemption;
            uint64 uche_trap_base;
        } msm;
        struct {
            uint32 address32_hi;
            uint32 has_vm_always_valid;
            char marketing_name[128];
        } amdgpu;
        struct {
            uint32 pci_bus;
            uint32 pci_dev;
            uint32 pci_func;
            uint32 pci_revision_id;
            uint32 pci_domain;
            uint32 pci_device_id;
        } intel;
    } u;
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
#define VIRTIO_GPU_ASYNC_MAX_DEPTH 8
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
    uint32 driver_features0;
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
    void *cursor_cmd_page;
    uint16 cursor_cmd_idx;
    uint16 cursor_ring;
    int cursor_x;
    int cursor_y;
    int cursor_visible;
    uint32 cursor_hot_x;
    uint32 cursor_hot_y;
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
    if (virtio_gpu_drain_async_submit_sample(g, 1, drain_sample) != 0)
        return -1;
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
            if (virtio_gpu_async_validate_retire(g, a) != 0)
                ret = -1;
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
                                      NULL, NULL, NULL);
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

static int virtio_gpu_user_get_drm_caps(uint32 requested_version, void *buf,
                                        uint32 buf_size, uint32 *capset_id,
                                        uint32 *capset_version,
                                        uint32 *capset_size)
{
    struct virtio_gpu_drm_capset caps;
    uint32 copy_size;

    if (requested_version > 1)
        return -EINVAL;

    memset(&caps, 0, sizeof(caps));
    caps.wire_format_version = 1;

    if (capset_id)
        *capset_id = VIRTIO_GPU_CAPSET_DRM;
    if (capset_version)
        *capset_version = 1;
    if (capset_size)
        *capset_size = sizeof(caps);
    if (buf == NULL || buf_size == 0)
        return 0;

    copy_size = sizeof(caps);
    if (copy_size > buf_size)
        copy_size = buf_size;
    memcpy(buf, &caps, copy_size);
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
        char buf[16];
        int depth = (virtio_gpu_cmdline_enabled("vgpu_async_pf") ||
                     virtio_gpu_cmdline_enabled(
                         "virtio_gpu_async_page_flip_scanout")) ? 8 : 2;

        if (cmdline_get_param("virtio_gpu_async_depth", buf,
                              sizeof(buf)) == 0 &&
            buf[0] >= '1' && buf[0] <= '0' + VIRTIO_GPU_ASYNC_MAX_DEPTH &&
            buf[1] == '\0')
            depth = buf[0] - '0';
        g->async_depth = depth;
    }
    return g->async_depth;
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
    int depth = virtio_gpu_async_depth(g);
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

static int virtio_gpu_backing_order(uint64 bytes, uint32 *alloc_len)
{
    uint64 len = PGSIZE;
    int order = 0;

    while (len < bytes && order < PAGE_BUDDY_MAX_ORDER) {
        order++;
        len <<= 1;
    }

    if (len < bytes || len > UINT32_MAX)
        return -1;

    *alloc_len = (uint32)len;
    return order;
}

static void virtio_gpu_release_pages(page_t **pages, uint32 npages)
{
    if (pages == NULL)
        return;
    for (uint32 i = 0; i < npages; i++) {
        if (pages[i] != NULL)
            page_ref_dec((void *)__page_to_pa(pages[i]));
    }
    kvfree(pages);
}

static int virtio_gpu_alloc_pages(uint32 npages, page_t ***pages_out)
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
            virtio_gpu_release_pages(pages, npages);
            return -ENOMEM;
        }
        memset((void *)PA2VA(__page_to_pa(pages[i])), 0, PGSIZE);
    }

    *pages_out = pages;
    return 0;
}

static int virtio_gpu_map_pages_current(page_t **pages, uint32 npages,
                                        uint64 size, uint64 *addr_out)
{
    vm_t *vm;
    vma_t *vma;
    uint64 addr;
    uint64 flags;
    uint64 pte_flags;

    if (pages == NULL || npages == 0 || size == 0 || addr_out == NULL ||
        current == NULL || current->vm == NULL)
        return -EINVAL;

    vm = current->vm;
    flags = PROT_READ | PROT_WRITE | VMA_FLAG_USER;

    vm_wlock(vm);
    addr = vm_find_free_range(vm, (size_t)size, 0);
    if (addr == 0) {
        vm_wunlock(vm);
        return -ENOMEM;
    }

    vma = vma_alloc(vm, addr, size, flags);
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
    for (uint32 i = 0; i < npages; i++) {
        uint64 va = addr + (uint64)i * PGSIZE;
        uint64 pa = __page_to_pa(pages[i]);

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
        page_add_anon_rmap(pages[i], vma, va);
    }

    vm_wunlock(vm);
    *addr_out = addr;
    return 0;
}

static int virtio_gpu_resource_create_2d(struct virtio_gpu *g, uint32 width,
                                         uint32 height, uint32 format,
                                         struct virtio_gpu_resource **out)
{
    struct virtio_gpu_resource_create_2d *create =
        (struct virtio_gpu_resource_create_2d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint64 bytes = (uint64)width * height * sizeof(uint32);
    uint32 alloc_len;

    if (width == 0 || height == 0 || bytes == 0 ||
        bytes / sizeof(uint32) / width != height) {
        virtio_gpu_count_failure(g);
        return -1;
    }
    int order = virtio_gpu_backing_order(bytes, &alloc_len);
    if (order < 0) {
        virtio_gpu_count_failure(g);
        printf("virtio_gpu: resource backing too large %ux%u bytes=%lu\n",
               width, height, bytes);
        return -1;
    }

    spin_lock(&g->lock);
    struct virtio_gpu_resource *res = virtio_gpu_alloc_resource_slot(g);
    if (res == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        return -1;
    }
    uint32 id = g->next_resource_id++;
    if (g->next_resource_id == 0)
        g->next_resource_id = 1;
    spin_unlock(&g->lock);

    void *backing = page_alloc(order, PAGE_TYPE_ANON);
    if (backing == NULL) {
        virtio_gpu_count_failure(g);
        printf("virtio_gpu: resource backing alloc failed order=%d bytes=%lu\n",
               order, bytes);
        return -1;
    }
    memset(backing, 0, alloc_len);

    memset(create, 0, sizeof(*create));
    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create->resource_id = id;
    create->format = format;
    create->width = width;
    create->height = height;
    if (virtio_gpu_submit(g, create, sizeof(*create), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0) {
        page_free(backing, order);
        return -1;
    }

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    res->in_use = 1;
    res->id = id;
    res->owner_tgid = 0;
    res->width = width;
    res->height = height;
    res->format = format;
    res->backing = backing;
    res->backing_len = (uint32)bytes;
    res->alloc_len = alloc_len;
    res->backing_order = (uint32)order;
    g->stats.resources++;
    g->stats.resource_bytes += bytes;
    spin_unlock(&g->lock);

    *out = res;
    return 0;
}

static int virtio_gpu_resource_create_3d(struct virtio_gpu *g,
                                         uint64 owner_id,
                                         pid_t owner_tgid,
                                         struct fb_gpu_virgl_resource_create *req,
                                         struct virtio_gpu_resource **out)
{
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint64 bytes = req->size;
    uint32 npages;
    page_t **pages = NULL;

    if (req->width == 0 || req->height == 0)
        return -EINVAL;
    if (bytes == 0)
        bytes = (uint64)req->width * req->height * sizeof(uint32);
    if (bytes == 0 || bytes > 64ULL * 1024 * 1024)
        return -EINVAL;
    npages = (uint32)PGROUNDUP(bytes) / PGSIZE;

    if (virtio_gpu_alloc_pages(npages, &pages) != 0)
        return -ENOMEM;

    spin_lock(&g->lock);
    struct virtio_gpu_resource *res = virtio_gpu_alloc_resource_slot(g);
    if (res == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        virtio_gpu_release_pages(pages, npages);
        return -ENOSPC;
    }
    uint32 id = g->next_resource_id++;
    if (g->next_resource_id == 0)
        g->next_resource_id = 1;
    spin_unlock(&g->lock);

    memset(create, 0, sizeof(*create));
    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = id;
    create->target = req->target;
    create->format = req->format;
    create->bind = req->bind;
    create->width = req->width;
    create->height = req->height;
    create->depth = req->depth ? req->depth : 1;
    create->array_size = req->array_size ? req->array_size : 1;
    create->last_level = req->last_level;
    create->nr_samples = req->nr_samples;
    create->flags = req->flags;
    if (virtio_gpu_submit(g, create, sizeof(*create), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0) {
        virtio_gpu_release_pages(pages, npages);
        return -EIO;
    }

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    res->in_use = 1;
    res->id = id;
    res->owner_id = owner_id;
    res->owner_tgid = owner_tgid;
    res->width = req->width;
    res->height = req->height;
    res->depth = create->depth;
    res->last_level = req->last_level;
    res->format = req->format;
    res->backing_len = (uint32)bytes;
    res->alloc_len = npages * PGSIZE;
    res->pages = pages;
    res->npages = npages;
    res->ctx_id = req->ctx_id;
    res->is_3d = 1;
    res->target = req->target;
    res->bind = req->bind;
    g->stats.resources++;
    g->stats.resource_bytes += bytes;
    spin_unlock(&g->lock);

    *out = res;
    return 0;
}

static int virtio_gpu_resource_create_3d_backing(struct virtio_gpu *g,
                                                 uint32 width,
                                                 uint32 height,
                                                 uint32 format,
                                                 uint32 bind,
                                                 struct virtio_gpu_resource **out)
{
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    struct virtio_gpu_resource *res;
    uint64 bytes = (uint64)width * height * sizeof(uint32);
    uint32 alloc_len = PGSIZE;
    int order = 0;
    void *backing;

    if (width == 0 || height == 0 || bytes == 0 ||
        bytes > 64ULL * 1024 * 1024)
        return -EINVAL;
    while (alloc_len < bytes) {
        if (order >= PAGE_BUDDY_MAX_ORDER)
            return -ENOMEM;
        order++;
        alloc_len <<= 1;
    }

    backing = page_alloc(order, PAGE_TYPE_ANON);
    if (backing == NULL)
        return -ENOMEM;

    spin_lock(&g->lock);
    res = virtio_gpu_alloc_resource_slot(g);
    if (res == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        page_free(backing, order);
        return -ENOSPC;
    }
    uint32 id = g->next_resource_id++;
    if (g->next_resource_id == 0)
        g->next_resource_id = 1;
    spin_unlock(&g->lock);

    memset(create, 0, sizeof(*create));
    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = id;
    create->target = VIRTIO_GPU_PIPE_TEXTURE_2D;
    create->format = format;
    create->bind = bind;
    create->width = width;
    create->height = height;
    create->depth = 1;
    create->array_size = 1;
    if (virtio_gpu_submit(g, create, sizeof(*create), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0) {
        page_free(backing, order);
        return -EIO;
    }

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    res->in_use = 1;
    res->id = id;
    res->owner_tgid = 0;
    res->width = width;
    res->height = height;
    res->depth = 1;
    res->format = format;
    res->backing = backing;
    res->backing_len = (uint32)bytes;
    res->alloc_len = alloc_len;
    res->backing_order = (uint32)order;
    res->is_3d = 1;
    res->target = VIRTIO_GPU_PIPE_TEXTURE_2D;
    res->bind = bind;
    g->stats.resources++;
    g->stats.resource_bytes += bytes;
    spin_unlock(&g->lock);

    *out = res;
    return 0;
}

static int virtio_gpu_resource_attach_backing(struct virtio_gpu *g,
                                              struct virtio_gpu_resource *res)
{
    struct virtio_gpu_resource_attach_backing *attach =
        (struct virtio_gpu_resource_attach_backing *)g->cmd_page;
    struct virtio_gpu_mem_entry *entry =
        (struct virtio_gpu_mem_entry *)g->data_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(entry, 0, sizeof(*entry));
    entry->addr = (uint64)res->backing;
    entry->length = res->backing_len;

    memset(attach, 0, sizeof(*attach));
    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = res->id;
    attach->nr_entries = 1;
    if (virtio_gpu_submit(g, attach, sizeof(*attach), entry, sizeof(*entry),
                          false, resp, sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -1;

    spin_lock(&g->lock);
    res->attached = 1;
    spin_unlock(&g->lock);
    return 0;
}

static int virtio_gpu_resource_attach_pages(struct virtio_gpu *g,
                                            struct virtio_gpu_resource *res)
{
    struct virtio_gpu_resource_attach_backing *attach =
        (struct virtio_gpu_resource_attach_backing *)g->cmd_page;
    struct virtio_gpu_mem_entry *entry;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 entries_len;
    uint32 alloc_len = PGSIZE;
    int order = 0;
    int ret;

    if (res->pages == NULL || res->npages == 0 ||
        res->npages > UINT32_MAX / sizeof(*entry))
        return -1;

    entries_len = res->npages * sizeof(*entry);
    while (alloc_len < entries_len) {
        if (order >= PAGE_BUDDY_MAX_ORDER)
            return -1;
        order++;
        alloc_len <<= 1;
    }

    entry = page_alloc(order, PAGE_TYPE_ANON);
    if (entry == NULL)
        return -1;

    memset(entry, 0, entries_len);
    for (uint32 i = 0; i < res->npages; i++) {
        entry[i].addr = __page_to_pa(res->pages[i]);
        entry[i].length = PGSIZE;
    }

    memset(attach, 0, sizeof(*attach));
    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = res->id;
    attach->nr_entries = res->npages;
    ret = virtio_gpu_submit(g, attach, sizeof(*attach), entry, entries_len,
                            false, resp, sizeof(*resp),
                            VIRTIO_GPU_RESP_OK_NODATA);
    page_free(entry, order);
    if (ret != 0)
        return -1;

    spin_lock(&g->lock);
    res->attached = 1;
    spin_unlock(&g->lock);
    return 0;
}

static int virtio_gpu_set_scanout(struct virtio_gpu *g, uint32 scanout_id,
                                  struct virtio_gpu_resource *res,
                                  uint32 x, uint32 y, uint32 width,
                                  uint32 height)
{
    struct virtio_gpu_set_scanout *set_scanout =
        (struct virtio_gpu_set_scanout *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(set_scanout, 0, sizeof(*set_scanout));
    set_scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    set_scanout->r.x = x;
    set_scanout->r.y = y;
    set_scanout->r.width = width;
    set_scanout->r.height = height;
    set_scanout->scanout_id = scanout_id;
    set_scanout->resource_id = res ? res->id : 0;

    return virtio_gpu_submit(g, set_scanout, sizeof(*set_scanout), NULL, 0,
                             false, resp, sizeof(*resp),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_set_scanout_async_prepare(
    uint32 scanout_id, uint32 resource_id, uint32 x, uint32 y, uint32 width,
    uint32 height, struct virtio_gpu_async_submit *prep)
{
    struct virtio_gpu_set_scanout *cmd;
    struct virtio_gpu_ctrl_hdr *resp;

    memset(prep, 0, sizeof(*prep));
    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->scanout_id = scanout_id;
    cmd->resource_id = resource_id;

    prep->ctx_id = 0;
    prep->fence_id = 0;
    prep->type = VIRTIO_GPU_CMD_SET_SCANOUT;
    prep->expected = VIRTIO_GPU_RESP_OK_NODATA;
    prep->cmd = cmd;
    prep->cmd_len = sizeof(*cmd);
    prep->data = NULL;
    prep->data_len = 0;
    prep->data_order = 0;
    prep->resp = resp;
    prep->resp_len = sizeof(*resp);
    return 0;
}

static int virtio_gpu_resource_transfer_2d(struct virtio_gpu *g,
                                           struct virtio_gpu_resource *res,
                                           uint32 x, uint32 y, uint32 width,
                                           uint32 height)
{
    struct virtio_gpu_transfer_to_host_2d *transfer =
        (struct virtio_gpu_transfer_to_host_2d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer->r.x = x;
    transfer->r.y = y;
    transfer->r.width = width;
    transfer->r.height = height;
    transfer->offset = (uint64)y * res->width * sizeof(uint32) +
                       (uint64)x * sizeof(uint32);
    transfer->resource_id = res->id;
    return virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                             resp, sizeof(*resp),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_resource_transfer_3d(struct virtio_gpu *g,
                                           struct virtio_gpu_resource *res,
                                           uint32 x, uint32 y, uint32 width,
                                           uint32 height)
{
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 stride;

    if (res == NULL || res->width == 0 || res->height == 0 ||
        width == 0 || height == 0)
        return -1;

    stride = res->width * sizeof(uint32);
    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    transfer->box.x = x;
    transfer->box.y = y;
    transfer->box.z = 0;
    transfer->box.w = width;
    transfer->box.h = height;
    transfer->box.d = 1;
    transfer->offset = (uint64)y * stride + (uint64)x * sizeof(uint32);
    transfer->resource_id = res->id;
    transfer->level = 0;
    transfer->stride = stride;
    transfer->layer_stride = (uint64)stride * res->height;

    return virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                             resp, sizeof(*resp),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_resource_transfer_scanout(struct virtio_gpu *g,
                                                struct virtio_gpu_resource *res,
                                                uint32 x, uint32 y,
                                                uint32 width, uint32 height)
{
    if (res != NULL && res->is_3d)
        return virtio_gpu_resource_transfer_3d(g, res, x, y, width, height);
    return virtio_gpu_resource_transfer_2d(g, res, x, y, width, height);
}

static int virtio_gpu_resource_flush(struct virtio_gpu *g,
                                     struct virtio_gpu_resource *res,
                                     uint32 x, uint32 y, uint32 width,
                                     uint32 height)
{
    struct virtio_gpu_resource_flush *flush =
        (struct virtio_gpu_resource_flush *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(flush, 0, sizeof(*flush));
    flush->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush->r.x = x;
    flush->r.y = y;
    flush->r.width = width;
    flush->r.height = height;
    flush->resource_id = res->id;
    return virtio_gpu_submit(g, flush, sizeof(*flush), NULL, 0, false, resp,
                             sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
}

/*
 * Fire-and-forget RESOURCE_FLUSH for the bound scanout resource.
 *
 * The synchronous virtio_gpu_resource_flush() path blocks the compositor's
 * present loop on the host's flush acknowledgement, which on the WSL D3D12
 * virgl host costs ~15ms per frame and serialises the desktop at roughly half
 * the application's render rate.  Linux/Alpine instead pipelines: it posts the
 * flush and continues servicing the event loop while the host composites in
 * parallel.  This routes the flush through the single-slot async submit so the
 * present returns immediately; the next ctrlq submission drains it (waiting for
 * the prior flush only if the host has not finished yet).  Gated by the
 * "virtio_gpu_async_scanout_flush" cmdline flag so the synchronous behaviour
 * remains the default.
 */
static int virtio_gpu_resource_flush_async(struct virtio_gpu *g,
                                           struct virtio_gpu_resource *res,
                                           uint32 x, uint32 y, uint32 width,
                                           uint32 height)
{
    struct virtio_gpu_resource_flush *cmd;
    struct virtio_gpu_ctrl_hdr *resp;
    struct virtio_gpu_async_submit prep;

    memset(&prep, 0, sizeof(prep));
    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->resource_id = res->id;

    prep.ctx_id = 0;
    prep.fence_id = 0;
    prep.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    prep.expected = VIRTIO_GPU_RESP_OK_NODATA;
    prep.cmd = cmd;
    prep.cmd_len = sizeof(*cmd);
    prep.data = NULL;
    prep.data_len = 0;
    prep.data_order = 0;
    prep.resp = resp;
    prep.resp_len = sizeof(struct virtio_gpu_ctrl_hdr);

    if (virtio_gpu_async_post_prepared(
            g, &prep, VIRTIO_GPU_ASYNC_REASON_FLUSH) != 0) {
        virtio_gpu_async_submit_free(&prep);
        return -1;
    }
    return 0;
}

static int virtio_gpu_resource_flush_async_prepare(
    uint32 resource_id, uint32 x, uint32 y, uint32 width, uint32 height,
    struct virtio_gpu_async_submit *prep)
{
    struct virtio_gpu_resource_flush *cmd;
    struct virtio_gpu_ctrl_hdr *resp;

    memset(prep, 0, sizeof(*prep));
    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->resource_id = resource_id;

    prep->ctx_id = 0;
    prep->fence_id = 0;
    prep->type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    prep->expected = VIRTIO_GPU_RESP_OK_NODATA;
    prep->cmd = cmd;
    prep->cmd_len = sizeof(*cmd);
    prep->data = NULL;
    prep->data_len = 0;
    prep->data_order = 0;
    prep->resp = resp;
    prep->resp_len = sizeof(struct virtio_gpu_ctrl_hdr);
    return 0;
}

/*
 * Fire-and-forget TRANSFER_TO_HOST_3D for the compositor's CPU-damaged chrome.
 *
 * The synchronous transfer path (virtio_gpu_user_transfer) routes through
 * virtio_gpu_submit(), which first drains the entire async ring -- so the small
 * per-frame chrome upload ends up blocking on the previous frame's RESOURCE_FLUSH
 * completing on the host (a ~vsync wait on the single in-order control queue).
 * Posting the transfer asynchronously lets the guest continue encoding the
 * gl-compose command stream while the host drains the queue, so host queue
 * processing overlaps guest CPU work.  The transfer references the resource's
 * already-attached backing pages (no inline data descriptor), exactly like the
 * synchronous TRANSFER_TO_HOST_3D command.  Gated by "virtio_gpu_async_fb_transfer".
 */
static int virtio_gpu_transfer_to_host_3d_async(struct virtio_gpu *g,
                                                struct virtio_gpu_resource *res,
                                                uint32 ctx_id, uint32 x,
                                                uint32 y, uint32 width,
                                                uint32 height, uint32 z,
                                                uint32 depth, uint32 level,
                                                uint64 offset, uint32 stride,
                                                uint32 layer_stride)
{
    struct virtio_gpu_async_submit *a;
    struct virtio_gpu_transfer_host_3d *cmd;
    struct virtio_gpu_ctrl_hdr *resp;
    uint32 transfer_stride;

    if (res == NULL || res->width == 0 || res->height == 0 ||
        width == 0 || height == 0 || depth == 0)
        return -1;

    if (virtio_gpu_async_make_room(g, VIRTIO_GPU_ASYNC_REASON_TRANSFER) != 0)
        return -1;

    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }

    transfer_stride = stride ? stride : res->width * sizeof(uint32);
    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    cmd->hdr.ctx_id = ctx_id;
    cmd->box.x = x;
    cmd->box.y = y;
    cmd->box.z = z;
    cmd->box.w = width;
    cmd->box.h = height;
    cmd->box.d = depth;
    cmd->offset = offset;
    cmd->resource_id = res->id;
    cmd->level = level;
    cmd->stride = transfer_stride;
    cmd->layer_stride = layer_stride ? layer_stride :
        (uint64)transfer_stride * res->height;

    a = virtio_gpu_async_reserve_slot(g);
    if (a == NULL) {
        kfree(cmd);
        kfree(resp);
        return -1;
    }
    a->ctx_id = ctx_id;
    a->fence_id = 0;
    a->type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    a->expected = VIRTIO_GPU_RESP_OK_NODATA;
    a->cmd = cmd;
    a->cmd_len = sizeof(*cmd);
    a->data = NULL;
    a->data_len = 0;
    a->data_order = 0;
    a->resp = resp;
    a->resp_len = sizeof(struct virtio_gpu_ctrl_hdr);

    virtio_gpu_async_post_locked(g, a);
    return 0;
}

static int virtio_gpu_resource_unref(struct virtio_gpu *g,
                                     struct virtio_gpu_resource *res)
{
    struct virtio_gpu_resource_unref *unref =
        (struct virtio_gpu_resource_unref *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 id = res->id;
    uint32 backing_len = res->backing_len;
    uint32 backing_order = res->backing_order;
    void *backing = res->backing;
    page_t **pages = res->pages;
    uint32 npages = res->npages;

    memset(unref, 0, sizeof(*unref));
    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = id;
    if (virtio_gpu_submit(g, unref, sizeof(*unref), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -1;

    spin_lock(&g->lock);
    memset(res, 0, sizeof(*res));
    if (g->stats.resources > 0)
        g->stats.resources--;
    if (g->stats.resource_bytes >= backing_len)
        g->stats.resource_bytes -= backing_len;
    spin_unlock(&g->lock);
    if (backing != NULL)
        page_free(backing, backing_order);
    virtio_gpu_release_pages(pages, npages);
    return 0;
}

static int virtio_gpu_get_display_info(struct virtio_gpu *g,
                                       uint32 *width, uint32 *height,
                                       int log)
{
    struct virtio_gpu_ctrl_hdr *cmd =
        (struct virtio_gpu_ctrl_hdr *)g->cmd_page;
    struct virtio_gpu_resp_display_info *resp =
        (struct virtio_gpu_resp_display_info *)g->resp_page;
    uint32 first_w = 0, first_h = 0;

    memset(cmd, 0, sizeof(*cmd));
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    if (virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                          sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_DISPLAY_INFO) != 0)
        return -1;

    if (log)
        printf("virtio_gpu: display info ok");
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (!resp->pmodes[i].enabled)
            continue;
        if (first_w == 0 || first_h == 0) {
            first_w = resp->pmodes[i].r.width;
            first_h = resp->pmodes[i].r.height;
        }
        if (log)
            printf(" scanout%d=%ux%u+%u+%u", i, resp->pmodes[i].r.width,
                   resp->pmodes[i].r.height, resp->pmodes[i].r.x,
                   resp->pmodes[i].r.y);
    }
    if (log)
        printf("\n");
    if (width != NULL)
        *width = first_w;
    if (height != NULL)
        *height = first_h;
    return 0;
}

static int virtio_gpu_submit_display_info(struct virtio_gpu *g)
{
    uint32 width = 0, height = 0;
    int ret = virtio_gpu_get_display_info(g, &width, &height, 1);

    if (ret == 0 && (g->scanout_width == 0 || g->scanout_height == 0)) {
        g->scanout_width = width;
        g->scanout_height = height;
    }
    return ret;
}

static int virtio_gpu_parse_edid_preferred(const uint8 *edid, uint32 size,
                                           uint32 *width, uint32 *height,
                                           uint32 *refresh_millihz)
{
    static const uint8 edid_header[8] =
        {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    uint8 sum = 0;

    if (edid == NULL || size < 128 || memcmp(edid, edid_header, 8) != 0)
        return -1;
    for (uint32 i = 0; i < 128; i++)
        sum = (uint8)(sum + edid[i]);
    if (sum != 0)
        return -1;

    for (uint32 off = 54; off + 18 <= 126; off += 18) {
        const uint8 *d = edid + off;
        uint32 pixel_clock = (uint32)d[0] | ((uint32)d[1] << 8);
        uint32 hactive;
        uint32 hblank;
        uint32 vactive;
        uint32 vblank;
        uint64 total;

        if (pixel_clock == 0)
            continue;
        hactive = (uint32)d[2] | (((uint32)d[4] & 0xf0) << 4);
        hblank = (uint32)d[3] | (((uint32)d[4] & 0x0f) << 8);
        vactive = (uint32)d[5] | (((uint32)d[7] & 0xf0) << 4);
        vblank = (uint32)d[6] | (((uint32)d[7] & 0x0f) << 8);
        if (hactive < 320 || hactive > 8192 ||
            vactive < 200 || vactive > 4320 ||
            hblank == 0 || vblank == 0)
            continue;
        total = (uint64)(hactive + hblank) * (uint64)(vactive + vblank);
        if (total == 0)
            continue;
        if (width != NULL)
            *width = hactive;
        if (height != NULL)
            *height = vactive;
        if (refresh_millihz != NULL)
            *refresh_millihz = (uint32)(((uint64)pixel_clock * 10000000ULL +
                                         total / 2) / total);
        return 0;
    }
    return -1;
}

static int virtio_gpu_get_edid_mode(struct virtio_gpu *g, uint32 *width,
                                    uint32 *height, uint32 *refresh_millihz,
                                    int log)
{
    struct virtio_gpu_cmd_get_edid *cmd =
        (struct virtio_gpu_cmd_get_edid *)g->cmd_page;
    struct virtio_gpu_resp_edid *resp =
        (struct virtio_gpu_resp_edid *)g->resp_page;
    uint32 w = 0, h = 0, hz = 0;

    if (!(g->driver_features0 & (1u << VIRTIO_GPU_F_EDID)))
        return -1;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_GET_EDID;
    cmd->scanout = 0;

    if (virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                          sizeof(*resp), VIRTIO_GPU_RESP_OK_EDID) != 0)
        return -1;
    if (resp->size > sizeof(resp->edid))
        resp->size = sizeof(resp->edid);
    if (virtio_gpu_parse_edid_preferred(resp->edid, resp->size,
                                        &w, &h, &hz) != 0)
        return -1;
    if (width != NULL)
        *width = w;
    if (height != NULL)
        *height = h;
    if (refresh_millihz != NULL)
        *refresh_millihz = hz;
    g->edid_width = w;
    g->edid_height = h;
    g->edid_refresh_millihz = hz;
    if (log)
        printf("virtio_gpu: edid preferred %ux%u@%u.%03uHz\n",
               w, h, hz / 1000, hz % 1000);
    return 0;
}

static int virtio_gpu_submit_capset(struct virtio_gpu *g, uint32 capset_id,
                                    uint32 version, uint32 max_size, void *out)
{
    struct virtio_gpu_get_capset *cmd =
        (struct virtio_gpu_get_capset *)g->cmd_page;
    struct virtio_gpu_resp_capset *resp =
        (struct virtio_gpu_resp_capset *)g->resp_page;
    uint32 data_len = max_size;

    if (data_len > PGSIZE - sizeof(*resp))
        data_len = PGSIZE - sizeof(*resp);

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, PGSIZE);
    cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd->capset_id = capset_id;
    cmd->capset_version = version;

    int ret = virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                                sizeof(*resp) + data_len,
                                VIRTIO_GPU_RESP_OK_CAPSET);
    if (ret == 0 && out != NULL)
        memcpy(out, resp->capset_data, data_len);
    return ret;
}

static void virtio_gpu_query_capsets(struct virtio_gpu *g)
{
    uint32 num_capsets = g->config->num_capsets;

    g->num_capsets = num_capsets;
    if (num_capsets == 0) {
        printf("virtio_gpu: no 3D capsets advertised\n");
        return;
    }

    printf("virtio_gpu: querying %u capset(s)\n", num_capsets);
    for (uint32 i = 0; i < num_capsets && i < VIRTIO_GPU_MAX_CAPSETS; i++) {
        struct virtio_gpu_get_capset_info *cmd =
            (struct virtio_gpu_get_capset_info *)g->cmd_page;
        struct virtio_gpu_resp_capset_info *resp =
            (struct virtio_gpu_resp_capset_info *)g->resp_page;

        memset(cmd, 0, sizeof(*cmd));
        memset(resp, 0, sizeof(*resp));
        cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        cmd->capset_index = i;

        if (virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                              sizeof(*resp),
                              VIRTIO_GPU_RESP_OK_CAPSET_INFO) != 0)
            continue;

        printf("virtio_gpu: capset[%u] id=%u version=%u size=%u\n",
               i, resp->capset_id, resp->capset_max_version,
               resp->capset_max_size);

        if (virtio_gpu_capset_supported(resp->capset_id)) {
            uint32 capset_id = resp->capset_id;
            uint32 capset_version = resp->capset_max_version;
            uint32 capset_size = resp->capset_max_size;

            if (virtio_gpu_submit_capset(g, capset_id, capset_version,
                                         capset_size, NULL) == 0) {
                spin_lock(&g->lock);
                g->capsets[i].valid = 1;
                g->capsets[i].id = capset_id;
                g->capsets[i].version = capset_version;
                g->capsets[i].size = capset_size;
                if (virtio_gpu_capset_preferred(g->virgl_capset_id,
                                                g->virgl_capset_version,
                                                capset_id, capset_version)) {
                    g->virgl_capset_id = capset_id;
                    g->virgl_capset_version = capset_version;
                    g->virgl_capset_size = capset_size;
                    g->stats.virgl = capset_id;
                    g->stats.virgl_version = capset_version;
                    g->stats.virgl_size = capset_size;
                }
                spin_unlock(&g->lock);
                printf("virtio_gpu: virgl capset ready id=%u version=%u size=%u\n",
                       capset_id, capset_version, capset_size);
            }
        }
    }

    if (num_capsets > VIRTIO_GPU_MAX_CAPSETS)
        printf("virtio_gpu: capset list truncated at %u entries\n",
               VIRTIO_GPU_MAX_CAPSETS);
    if (g->virgl_capset_id == 0)
        printf("virtio_gpu: no virgl capset found\n");
}

static int virtio_gpu_create_context(struct virtio_gpu *g, uint32 ctx_id,
                                     uint32 capset_id, const char *name)
{
    struct virtio_gpu_ctx_create *cmd =
        (struct virtio_gpu_ctx_create *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 name_len = 0;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd->hdr.ctx_id = ctx_id;
    if (g->driver_features0 & (1u << VIRTIO_GPU_F_CONTEXT_INIT))
        cmd->context_init = capset_id;
    while (name[name_len] && name_len < sizeof(cmd->debug_name))
        name_len++;
    memcpy(cmd->debug_name, name, name_len);
    cmd->nlen = name_len;

    return virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                             sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_destroy_context(struct virtio_gpu *g, uint32 ctx_id)
{
    struct virtio_gpu_ctx_destroy *cmd =
        (struct virtio_gpu_ctx_destroy *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd->hdr.ctx_id = ctx_id;

    return virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                             sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_context_resource(struct virtio_gpu *g, uint32 type,
                                       uint32 ctx_id, uint32 resource_id)
{
    struct virtio_gpu_ctx_resource *cmd =
        (struct virtio_gpu_ctx_resource *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = type;
    cmd->hdr.ctx_id = ctx_id;
    cmd->resource_id = resource_id;

    return virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                             sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_submit_3d(struct virtio_gpu *g, uint32 ctx_id,
                                const uint32 *cmds, uint32 nr_dwords,
                                uint64 fence_id)
{
    struct virtio_gpu_cmd_submit *cmd =
        (struct virtio_gpu_cmd_submit *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 bytes = nr_dwords * sizeof(uint32);

    if (nr_dwords == 0 || bytes > PGSIZE * 64) {
        virtio_gpu_count_failure(g);
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = ctx_id;
    if (fence_id != 0) {
        cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        cmd->hdr.fence_id = fence_id;
    }
    cmd->size = bytes;

    if (virtio_gpu_submit(g, cmd, sizeof(*cmd), (void *)cmds, bytes, false,
                          resp, sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -1;

    if (fence_id != 0) {
        if (resp->fence_id != fence_id) {
            virtio_gpu_count_failure(g);
            printf("virtio_gpu: fence mismatch got=%lu expected=%lu\n",
                   resp->fence_id, fence_id);
            return -1;
        }
        spin_lock(&g->lock);
        g->stats.fences++;
        g->stats.last_fence = fence_id;
        spin_unlock(&g->lock);
    }
    return 0;
}

static int virtio_gpu_submit_3d_async_prepare(
    struct virtio_gpu *g, uint32 ctx_id, const uint32 *cmds,
    uint32 nr_dwords, uint64 fence_id, struct virtio_gpu_async_submit *prep)
{
    struct virtio_gpu_cmd_submit *cmd;
    struct virtio_gpu_ctrl_hdr *resp;
    uint32 bytes = nr_dwords * sizeof(uint32);
    uint32 order;
    void *data;

    memset(prep, 0, sizeof(*prep));
    if (nr_dwords == 0 || bytes > PGSIZE * 64) {
        virtio_gpu_count_failure(g);
        return -1;
    }
    {
        uint32 alloc_len;
        int backing_order = virtio_gpu_backing_order(bytes, &alloc_len);
        if (backing_order < 0)
            return -1;
        order = (uint32)backing_order;
    }

    cmd = kalloc();
    resp = kalloc();
    if (cmd == NULL || resp == NULL) {
        if (cmd != NULL)
            kfree(cmd);
        if (resp != NULL)
            kfree(resp);
        return -ENOMEM;
    }
    data = page_alloc(order, PAGE_TYPE_ANON);
    if (data == NULL) {
        kfree(cmd);
        kfree(resp);
        return -ENOMEM;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    memcpy(data, cmds, bytes);
    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = ctx_id;
    if (fence_id != 0) {
        cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        cmd->hdr.fence_id = fence_id;
    }
    cmd->size = bytes;

    prep->ctx_id = ctx_id;
    prep->fence_id = fence_id;
    prep->type = VIRTIO_GPU_CMD_SUBMIT_3D;
    prep->expected = VIRTIO_GPU_RESP_OK_NODATA;
    prep->cmd = cmd;
    prep->cmd_len = sizeof(*cmd);
    prep->data = data;
    prep->data_len = bytes;
    prep->data_order = order;
    prep->resp = resp;
    prep->resp_len = sizeof(*resp);
    return 0;
}

static int virtio_gpu_submit_3d_async_post_prepared(
    struct virtio_gpu *g, struct virtio_gpu_async_submit *prep)
{
    return virtio_gpu_async_post_prepared(
        g, prep, VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D);
}

static void virtio_gpu_smoke_context(struct virtio_gpu *g)
{
    uint32 ctx_id = 1;
    uint32 nop = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    uint64 fence_id;
    struct virtio_gpu_resource *res = NULL;
    int created = 0;
    int attached_to_ctx = 0;

    if (!g->virgl_capset_id)
        return;
    if (!(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL))) {
        printf("virtio_gpu: virgl capset present but feature not negotiated\n");
        return;
    }

    if (virtio_gpu_create_context(g, ctx_id, g->virgl_capset_id,
                                  "xv6-virgl-smoke") != 0) {
        printf("virtio_gpu: 3D context create failed\n");
        return;
    }
    created = 1;
    if (virtio_gpu_resource_create_2d(g, VIRTIO_GPU_SMOKE_WIDTH,
                                      VIRTIO_GPU_SMOKE_HEIGHT,
                                      VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                      &res) != 0) {
        printf("virtio_gpu: 3D context resource create failed\n");
        goto out;
    }
    if (virtio_gpu_resource_attach_backing(g, res) != 0) {
        printf("virtio_gpu: 3D context resource backing failed\n");
        goto out;
    }
    if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                    ctx_id, res->id) != 0) {
        printf("virtio_gpu: 3D context attach resource failed\n");
        goto out;
    }
    attached_to_ctx = 1;

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (virtio_gpu_submit_3d(g, ctx_id, &nop, 1, fence_id) != 0) {
        printf("virtio_gpu: 3D NOP submit failed\n");
        goto out;
    }

    if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    ctx_id, res->id) != 0) {
        printf("virtio_gpu: 3D context detach resource failed\n");
        goto out;
    }
    attached_to_ctx = 0;
    if (virtio_gpu_destroy_context(g, ctx_id) != 0) {
        printf("virtio_gpu: 3D context destroy failed\n");
        goto out;
    }
    created = 0;
    printf("virtio_gpu: 3D context smoke ok ctx=%u capset=%u resource=%u fence=%lu\n",
           ctx_id, g->virgl_capset_id, res ? res->id : 0, fence_id);

out:
    if (attached_to_ctx && res)
        virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    ctx_id, res->id);
    if (res)
        virtio_gpu_resource_unref(g, res);
    if (created)
        virtio_gpu_destroy_context(g, ctx_id);
}

int virtio_gpu_user_context_create(uint64 owner_id, pid_t owner_tgid,
                                   uint32 capset_id, const char *name,
                                   uint32 *ctx_id)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_capset *capset;
    uint32 id;
    int ret;

    if (ctx_id == NULL)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_CONTEXT);
    spin_lock(&g->lock);
    if (capset_id == 0)
        capset_id = g->virgl_capset_id;
    capset = virtio_gpu_lookup_capset_locked(g, capset_id);
    if (capset == NULL) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EINVAL;
    }
    ctx = virtio_gpu_alloc_context_slot_locked(g);
    if (ctx == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOSPC;
    }
    id = g->next_context_id++;
    if (g->next_context_id == 0)
        g->next_context_id = 2;
    memset(ctx, 0, sizeof(*ctx));
    ctx->in_use = 1;
    ctx->id = id;
    ctx->capset_id = capset_id;
    ctx->owner_id = owner_id;
    ctx->owner_tgid = owner_tgid;
    spin_unlock(&g->lock);

    ret = virtio_gpu_create_context(g, id, capset_id,
                                    name ? name : "xv6-virgl");
    if (ret != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, id);
        if (ctx != NULL)
            memset(ctx, 0, sizeof(*ctx));
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EIO;
    }
    virtio_gpu_op_unlock(g);

    *ctx_id = id;
    return 0;
}

int virtio_gpu_user_context_destroy(uint64 owner_id, pid_t owner_tgid,
                                    uint32 ctx_id)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    int ret;

    if (ctx_id == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_CONTEXT);
    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx == NULL) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EPERM;
    }
    spin_unlock(&g->lock);

    ret = virtio_gpu_destroy_context(g, ctx_id);
    if (ret == 0) {
        spin_lock(&g->lock);
        if (g->present_scanout_ctx_id == ctx_id) {
            g->present_scanout_ctx_id = 0;
            g->present_scanout_resource_id = 0;
        }
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
        if (ctx != NULL) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_submit(uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
                           uint32 flags, const uint32 *cmds,
                           uint32 nr_dwords, const uint32 *resources,
                           uint32 resource_count, uint64 *fence,
                           uint64 *signaled)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_async_submit prep;
    uint64 fence_id;
    int ret;
    int async_submit;

    if ((flags & ~(FB_GPU_VIRGL_SUBMIT_ASYNC |
                   FB_GPU_VIRGL_SUBMIT_FORCE_FAIL)) != 0 ||
        ctx_id == 0 || cmds == NULL || nr_dwords == 0 ||
        nr_dwords > (PGSIZE * 64) / sizeof(uint32))
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx == NULL) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -EPERM;
    }
    if (ctx->failed) {
        spin_unlock(&g->lock);
        return -EIO;
    }
    if (flags & FB_GPU_VIRGL_SUBMIT_FORCE_FAIL) {
        virtio_gpu_mark_context_failed_locked(g, ctx);
        spin_unlock(&g->lock);
        return -EIO;
    }
    spin_unlock(&g->lock);

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);

    async_submit = (flags & FB_GPU_VIRGL_SUBMIT_ASYNC) != 0;
    memset(&prep, 0, sizeof(prep));
    if (async_submit) {
        ret = virtio_gpu_submit_3d_async_prepare(
            g, ctx_id, cmds, nr_dwords, fence_id, &prep);
        if (ret != 0)
            return ret;
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_SUBMIT_3D);
    if (async_submit)
        ret = virtio_gpu_submit_3d_async_post_prepared(g, &prep);
    else
        ret = virtio_gpu_submit_3d(g, ctx_id, cmds, nr_dwords, fence_id);
    virtio_gpu_op_unlock(g);
    if (ret != 0)
        virtio_gpu_async_submit_free(&prep);
    if (ret != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
        virtio_gpu_mark_context_failed_locked(g, ctx);
        spin_unlock(&g->lock);
        return -EIO;
    }

    spin_lock(&g->lock);
    if (signaled)
        *signaled = g->stats.last_fence;
    if (resources != NULL) {
        for (uint32 i = 0; i < resource_count; i++) {
            struct virtio_gpu_resource *res;

            if (resources[i] == 0)
                continue;
            res = virtio_gpu_lookup_resource_locked(g, resources[i]);
            if (res == NULL)
                continue;
            if (!virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                          owner_id, owner_tgid))
                continue;
            res->last_submit_fence = fence_id;
        }
    }
    spin_unlock(&g->lock);
    if (fence)
        *fence = fence_id;
    return 0;
}

int virtio_gpu_user_fence(uint64 wait_for, int wait, uint64 *signaled)
{
    struct virtio_gpu *g = &gpu;
    uint64 done;
    int ret = 0;

    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_FENCE);
    spin_lock(&g->lock);
    done = g->stats.last_fence;
    spin_unlock(&g->lock);
    if (virtio_gpu_async_pending(g) && wait && wait_for != 0) {
        ret = virtio_gpu_drain_async_until_fence(g, wait_for);
        if (ret != 0) {
            virtio_gpu_op_unlock(g);
            return -EIO;
        }
    } else if (virtio_gpu_async_pending(g) &&
               (wait || wait_for == 0 ||
                wait_for <= virtio_gpu_async_newest_fence(g))) {
        ret = virtio_gpu_drain_async_submit(g, wait);
        if (ret != 0) {
            virtio_gpu_op_unlock(g);
            return -EIO;
        }
    }
    spin_lock(&g->lock);
    done = g->stats.last_fence;
    spin_unlock(&g->lock);
    virtio_gpu_op_unlock(g);

    if (signaled)
        *signaled = done;
    if (wait && wait_for != 0 && wait_for > done)
        return -EAGAIN;
    return 0;
}

int virtio_gpu_user_get_caps_for(uint32 requested_capset_id,
                                 uint32 requested_capset_version,
                                 void *buf, uint32 buf_size,
                                 uint32 *capset_id, uint32 *capset_version,
                                 uint32 *capset_size)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_capset *capset;
    uint32 id;
    uint32 version;
    uint32 size;
    uint32 transfer_size;
    int ret = 0;

    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    if (requested_capset_id == VIRTIO_GPU_CAPSET_DRM)
        return virtio_gpu_user_get_drm_caps(requested_capset_version, buf,
                                           buf_size, capset_id,
                                           capset_version, capset_size);

    spin_lock(&g->lock);
    if (requested_capset_id == 0)
        requested_capset_id = g->virgl_capset_id;
    capset = virtio_gpu_lookup_capset_locked(g, requested_capset_id);
    if (capset == NULL) {
        spin_unlock(&g->lock);
        return -EINVAL;
    }
    id = capset->id;
    version = capset->version;
    size = capset->size;
    spin_unlock(&g->lock);

    if (requested_capset_version != 0) {
        if (requested_capset_version > version)
            return -EINVAL;
        version = requested_capset_version;
    }

    if (capset_id)
        *capset_id = id;
    if (capset_version)
        *capset_version = version;
    if (capset_size)
        *capset_size = size;
    if (buf == NULL || buf_size == 0)
        return 0;
    if (size == 0)
        return -EINVAL;
    transfer_size = size;
    if (transfer_size > buf_size)
        transfer_size = buf_size;
    if (transfer_size > PGSIZE - sizeof(struct virtio_gpu_resp_capset))
        transfer_size = PGSIZE - sizeof(struct virtio_gpu_resp_capset);

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_CAPSET);
    ret = virtio_gpu_submit_capset(g, id, version, transfer_size, buf);
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_get_caps(void *buf, uint32 buf_size, uint32 *capset_id,
                             uint32 *capset_version, uint32 *capset_size)
{
    return virtio_gpu_user_get_caps_for(0, 0, buf, buf_size, capset_id,
                                        capset_version, capset_size);
}

int virtio_gpu_user_capset_ids(uint64 *ids)
{
    struct virtio_gpu *g = &gpu;
    uint64 value = 0;

    if (ids == NULL)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    spin_lock(&g->lock);
    for (int i = 0; i < VIRTIO_GPU_MAX_CAPSETS; i++) {
        if (g->capsets[i].valid && g->capsets[i].id < 64)
            value |= 1ULL << g->capsets[i].id;
    }
    spin_unlock(&g->lock);
    *ids = value;
    return 0;
}

int virtio_gpu_user_resource_create(uint64 owner_id, pid_t owner_tgid,
                                    struct fb_gpu_virgl_resource_create *req)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_resource *res = NULL;
    uint64 addr;
    int ret;
    int attached_to_ctx = 0;

    if (req == NULL)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    if (req->ctx_id != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, req->ctx_id);
        if (ctx == NULL) {
            spin_unlock(&g->lock);
            return -ENOENT;
        }
        if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                      owner_id, owner_tgid)) {
            spin_unlock(&g->lock);
            return -EPERM;
        }
        if (ctx->failed) {
            spin_unlock(&g->lock);
            return -EIO;
        }
        spin_unlock(&g->lock);
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    ret = virtio_gpu_resource_create_3d(g, owner_id, owner_tgid, req, &res);
    if (ret != 0)
        goto out;
    if (virtio_gpu_resource_attach_pages(g, res) != 0) {
        ret = -EIO;
        goto out_unref;
    }
    if (req->ctx_id != 0) {
        if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                        req->ctx_id, res->id) != 0) {
            ret = -EIO;
            goto out_unref;
        }
        attached_to_ctx = 1;
    }

    ret = virtio_gpu_map_pages_current(res->pages, res->npages,
                                       res->alloc_len, &addr);
    if (ret != 0)
        goto out_detach;

    req->resource_id = res->id;
    req->size = res->alloc_len;
    req->addr = addr;
    virtio_gpu_op_unlock(g);
    return 0;

out_detach:
    if (attached_to_ctx)
        virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    req->ctx_id, res->id);
out_unref:
    if (res != NULL)
        virtio_gpu_resource_unref(g, res);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

static int virtio_gpu_destroy_resource_locked(struct virtio_gpu *g,
                                              uint32 resource_id,
                                              uint64 owner_id,
                                              pid_t owner_tgid)
{
    struct virtio_gpu_resource *res;
    uint32 ctx_id;
    int ret;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    ctx_id = res ? res->ctx_id : 0;
    if (res != NULL &&
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid))
        res = NULL;
    spin_unlock(&g->lock);
    if (res == NULL)
        return -ENOENT;

    spin_lock(&g->lock);
    if (res->export_refs != 0) {
        res->destroy_pending = 1;
        spin_unlock(&g->lock);
        return 0;
    }
    spin_unlock(&g->lock);

    if (ctx_id != 0)
        virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    ctx_id, resource_id);
    virtio_gpu_page_flip_scanout_set_remove(g, resource_id);
    ret = virtio_gpu_resource_unref(g, res);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_resource_destroy(uint64 owner_id, pid_t owner_tgid,
                                     uint32 resource_id)
{
    struct virtio_gpu *g = &gpu;
    int ret;

    if (resource_id == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    ret = virtio_gpu_destroy_resource_locked(g, resource_id, owner_id,
                                             owner_tgid);
    virtio_gpu_op_unlock(g);
    return ret;
}

int virtio_gpu_user_resource_attach(uint64 owner_id, pid_t owner_tgid,
                                    uint32 ctx_id, uint32 resource_id,
                                    int allow_imported)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_resource *res;
    int ret;

    if (ctx_id == 0 || resource_id == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx == NULL) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EPERM;
    }
    if (ctx->failed) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EIO;
    }
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid) &&
        !allow_imported) {
        spin_unlock(&g->lock);
        virtio_gpu_op_unlock(g);
        return -EPERM;
    }
    spin_unlock(&g->lock);

    ret = virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                      ctx_id, resource_id);
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_resource_export_pages(uint64 owner_id, pid_t owner_tgid,
                                          uint32 resource_id, uint32 *width,
                                          uint32 *height, uint32 *pitch,
                                          uint64 *size, page_t ***pages_out,
                                          uint32 *npages_out)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    page_t **pages;
    uint32 npages;
    uint32 i;

    if (resource_id == 0 || width == NULL || height == NULL ||
        pitch == NULL || size == NULL || pages_out == NULL ||
        npages_out == NULL)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL ||
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (res->pages == NULL || res->npages == 0 ||
        res->width == 0 || res->height == 0 ||
        res->width > 0xffffffffU / 4) {
        spin_unlock(&g->lock);
        return -EINVAL;
    }

    npages = res->npages;
    pages = kvmalloc((size_t)npages * sizeof(*pages));
    if (pages == NULL) {
        spin_unlock(&g->lock);
        return -ENOMEM;
    }
    memset(pages, 0, (size_t)npages * sizeof(*pages));

    for (i = 0; i < npages; i++) {
        uint64 pa;

        if (res->pages[i] == NULL)
            goto fail_locked;
        pa = __page_to_pa(res->pages[i]);
        if (page_ref_inc((void *)pa) <= 0)
            goto fail_locked;
        pages[i] = res->pages[i];
    }

    *width = res->width;
    *height = res->height;
    *pitch = res->width * 4;
    *size = res->alloc_len;
    *pages_out = pages;
    *npages_out = npages;
    res->export_refs++;
    spin_unlock(&g->lock);
    return 0;

fail_locked:
    while (i > 0) {
        i--;
        if (pages[i] != NULL)
            page_ref_dec((void *)__page_to_pa(pages[i]));
    }
    kvfree(pages);
    spin_unlock(&g->lock);
    return -ENOMEM;
}

void virtio_gpu_user_resource_export_put(uint32 resource_id)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    uint32 ctx_id = 0;
    int destroy_now = 0;

    if (resource_id == 0 || !g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res != NULL) {
        if (res->export_refs > 0)
            res->export_refs--;
        if (res->export_refs == 0 && res->destroy_pending) {
            ctx_id = res->ctx_id;
            destroy_now = 1;
        }
    }
    spin_unlock(&g->lock);

    if (destroy_now && res != NULL) {
        if (ctx_id != 0)
            virtio_gpu_context_resource(g,
                                        VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                        ctx_id, resource_id);
        (void)virtio_gpu_resource_unref(g, res);
    }
    virtio_gpu_op_unlock(g);
}

int virtio_gpu_user_resource_info(uint64 owner_id, pid_t owner_tgid,
                                  uint32 resource_id, uint32 *width,
                                  uint32 *height, uint32 *format,
                                  uint64 *size)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;

    if (resource_id == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL ||
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (width)
        *width = res->width;
    if (height)
        *height = res->height;
    if (format)
        *format = res->format;
    if (size)
        *size = res->alloc_len;
    spin_unlock(&g->lock);
    return 0;
}

int virtio_gpu_user_resource_last_submit_fence(uint64 owner_id,
                                               pid_t owner_tgid,
                                               uint32 resource_id,
                                               uint64 *fence)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;

    if (fence)
        *fence = 0;
    if (resource_id == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL ||
        !virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        return -ENOENT;
    }
    if (fence)
        *fence = res->last_submit_fence;
    spin_unlock(&g->lock);
    return 0;
}

void *virtio_gpu_user_resource_page(uint64 owner_id, pid_t owner_tgid,
                                    uint32 resource_id, uint64 page_index)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    void *pa = NULL;

    if (resource_id == 0 || !g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return NULL;

    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res != NULL &&
        virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                 owner_id, owner_tgid) &&
        res->pages != NULL && page_index < res->npages &&
        res->pages[page_index] != NULL) {
        pa = (void *)__page_to_pa(res->pages[page_index]);
        if (page_ref_inc(pa) <= 0)
            pa = NULL;
    }
    spin_unlock(&g->lock);
    return pa;
}

void virtio_gpu_user_destroy_owner(pid_t owner_tgid)
{
    struct virtio_gpu *g = &gpu;
    uint32 *resource_ids;
    uint32 *context_ids;
    uint32 nresources = 0;
    uint32 ncontexts = 0;

    if (owner_tgid == 0)
        return;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return;

    resource_ids = kvmalloc((size_t)VIRTIO_GPU_MAX_RESOURCES *
                            sizeof(*resource_ids));
    context_ids = kvmalloc((size_t)VIRTIO_GPU_MAX_CONTEXTS *
                           sizeof(*context_ids));
    if (resource_ids == NULL || context_ids == NULL) {
        kvfree(resource_ids);
        kvfree(context_ids);
        return;
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (g->resources[i].in_use &&
            g->resources[i].owner_tgid == owner_tgid &&
            nresources < VIRTIO_GPU_MAX_RESOURCES)
            resource_ids[nresources++] = g->resources[i].id;
    }
    for (int i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        if (g->contexts[i].in_use &&
            g->contexts[i].owner_tgid == owner_tgid &&
            ncontexts < VIRTIO_GPU_MAX_CONTEXTS)
            context_ids[ncontexts++] = g->contexts[i].id;
    }
    spin_unlock(&g->lock);

    for (uint32 i = 0; i < nresources; i++)
        (void)virtio_gpu_destroy_resource_locked(g, resource_ids[i], 0,
                                                 owner_tgid);

    for (uint32 i = 0; i < ncontexts; i++) {
        struct virtio_gpu_context *ctx;

        if (virtio_gpu_destroy_context(g, context_ids[i]) != 0)
            continue;
        spin_lock(&g->lock);
        if (g->present_scanout_ctx_id == context_ids[i]) {
            g->present_scanout_ctx_id = 0;
            g->present_scanout_resource_id = 0;
        }
        ctx = virtio_gpu_lookup_context_locked(g, context_ids[i]);
        if (ctx != NULL && ctx->owner_tgid == owner_tgid) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    virtio_gpu_op_unlock(g);
    kvfree(resource_ids);
    kvfree(context_ids);
}

void virtio_gpu_user_destroy_render_owner(uint64 owner_id)
{
    struct virtio_gpu *g = &gpu;
    uint32 *resource_ids;
    uint32 *context_ids;
    uint32 nresources = 0;
    uint32 ncontexts = 0;

    if (owner_id == 0)
        return;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return;

    resource_ids = kvmalloc((size_t)VIRTIO_GPU_MAX_RESOURCES *
                            sizeof(*resource_ids));
    context_ids = kvmalloc((size_t)VIRTIO_GPU_MAX_CONTEXTS *
                           sizeof(*context_ids));
    if (resource_ids == NULL || context_ids == NULL) {
        kvfree(resource_ids);
        kvfree(context_ids);
        return;
    }

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESOURCE);
    spin_lock(&g->lock);
    for (int i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (g->resources[i].in_use &&
            g->resources[i].owner_id == owner_id &&
            nresources < VIRTIO_GPU_MAX_RESOURCES)
            resource_ids[nresources++] = g->resources[i].id;
    }
    for (int i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        if (g->contexts[i].in_use &&
            g->contexts[i].owner_id == owner_id &&
            ncontexts < VIRTIO_GPU_MAX_CONTEXTS)
            context_ids[ncontexts++] = g->contexts[i].id;
    }
    spin_unlock(&g->lock);

    for (uint32 i = 0; i < nresources; i++)
        (void)virtio_gpu_destroy_resource_locked(g, resource_ids[i],
                                                 owner_id, 0);

    for (uint32 i = 0; i < ncontexts; i++) {
        struct virtio_gpu_context *ctx;

        if (virtio_gpu_destroy_context(g, context_ids[i]) != 0)
            continue;
        spin_lock(&g->lock);
        if (g->present_scanout_ctx_id == context_ids[i]) {
            g->present_scanout_ctx_id = 0;
            g->present_scanout_resource_id = 0;
        }
        ctx = virtio_gpu_lookup_context_locked(g, context_ids[i]);
        if (ctx != NULL && ctx->owner_id == owner_id) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    virtio_gpu_op_unlock(g);
    kvfree(resource_ids);
    kvfree(context_ids);
}

int virtio_gpu_user_transfer(uint64 owner_id, pid_t owner_tgid,
                             struct fb_gpu_virgl_transfer *req,
                             int from_host)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    struct virtio_gpu_transfer_host_3d *cmd =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 cmd_type = from_host ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D :
                                  VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    uint32 ctx_id = 0;
    int ret = -EIO;
    int transfer_perf;
    int async_path = 0;
    int mixed_path = 0;
    int linear_transfer_path = 0;
    int bound_scanout_path = 0;
    int wait_holder = VIRTIO_GPU_OP_NONE;
    uint64 total_start = 0;
    uint64 lock_acquired = 0;
    uint64 submit_start;
    uint64 submit_ticks = 0;
    uint64 drain_ticks = 0;
    uint64 command_ticks = 0;
    struct virtio_gpu_async_drain_sample drain_sample;
    uint32 log_x = 0;
    uint32 log_y = 0;
    uint32 log_w = 0;
    uint32 log_h = 0;
    uint32 log_resource_id = 0;
    uint32 log_ctx_id = 0;
    uint32 log_res_width = 0;
    uint32 log_res_height = 0;
    uint32 log_target = 0;
    uint32 log_bind = 0;
    uint64 log_offset = 0;
    uint32 log_stride = 0;

    if (req == NULL || req->resource_id == 0 || req->flags != 0 ||
        req->w == 0 || req->h == 0 || req->d == 0)
        return -EINVAL;
    memset(&drain_sample, 0, sizeof(drain_sample));
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;
    log_x = req->x;
    log_y = req->y;
    log_w = req->w;
    log_h = req->h;
    log_resource_id = req->resource_id;
    log_offset = req->offset;
    log_stride = req->stride;

    transfer_perf = virtio_gpu_cmdline_enabled("virtio_gpu_transfer_perf");
    if (transfer_perf)
        total_start = r_time();
    if (transfer_perf && mutex_trylock(&g->op_lock)) {
        wait_holder = VIRTIO_GPU_OP_NONE;
    } else {
        if (transfer_perf)
            wait_holder = __atomic_load_n(&g->op_lock_holder,
                                          __ATOMIC_RELAXED);
        mutex_lock(&g->op_lock);
    }
    __atomic_store_n(&g->op_lock_holder, VIRTIO_GPU_OP_TRANSFER,
                     __ATOMIC_RELAXED);
    if (transfer_perf)
        lock_acquired = r_time();
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, req->resource_id);
    if (res != NULL) {
        struct virtio_gpu_context *ctx = NULL;
        if (res->ctx_id != 0)
            ctx = virtio_gpu_lookup_context_locked(g, res->ctx_id);
        if (!virtio_gpu_owner_matches(res->owner_id, res->owner_tgid,
                                      owner_id, owner_tgid) ||
            (ctx != NULL && ctx->failed) ||
            req->x > res->width || req->w > res->width - req->x ||
            req->y > res->height || req->h > res->height - req->y ||
            req->z > res->depth || req->d > res->depth - req->z ||
            req->level > res->last_level)
            res = NULL;
        else
            ctx_id = res->ctx_id;
    }
    spin_unlock(&g->lock);
    if (res == NULL) {
        ret = -EINVAL;
        goto out;
    }
    bound_scanout_path = req->resource_id == g->bound_scanout_resource_id;
    log_ctx_id = ctx_id;
    log_res_width = res->width;
    log_res_height = res->height;
    log_target = res->target;
    log_bind = res->bind;
    linear_transfer_path = !from_host && res->height == 1 && req->h == 1 &&
        res->target == 0 && (res->bind & 0x20) != 0;

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = cmd_type;
    cmd->hdr.ctx_id = ctx_id;
    cmd->box.x = req->x;
    cmd->box.y = req->y;
    cmd->box.z = req->z;
    cmd->box.w = req->w;
    cmd->box.h = req->h;
    cmd->box.d = req->d;
    cmd->offset = req->offset;
    cmd->resource_id = req->resource_id;
    cmd->level = req->level;
    cmd->stride = req->stride ? req->stride : res->width * sizeof(uint32);
    cmd->layer_stride = req->layer_stride ? req->layer_stride :
        cmd->stride * res->height;

    /*
     * The compositor's per-frame chrome upload to the bound scanout resource is
     * small and on the present hot path.  The synchronous submit below first
     * drains the whole async ring, so the upload ends up blocking on the prior
     * frame's RESOURCE_FLUSH completing (a ~vsync wait on the single in-order
     * control queue).  Posting it asynchronously instead lets the guest keep
     * encoding the gl-compose stream while the host drains the queue.  Only
     * TO_HOST transfers of the bound scanout resource are eligible; FROM_HOST
     * readbacks and other resources keep the synchronous semantics callers
     * rely on.  Opt-in via the "virtio_gpu_async_fb_transfer" cmdline flag.
     *
     * Mesa's linear transfer resources are a separate diagnostic path: they use
     * a 1D resource layout and must preserve the caller's offset/stride exactly.
     * Keep that behind "virtio_gpu_async_linear_transfer" until validation proves
     * it is safe and useful.
     */
    if (!from_host &&
        ((req->resource_id == g->bound_scanout_resource_id &&
          virtio_gpu_cmdline_enabled("virtio_gpu_async_fb_transfer")) ||
         (res->height == 1 && req->h == 1 && res->target == 0 &&
          (res->bind & 0x20) != 0 &&
          virtio_gpu_cmdline_enabled("virtio_gpu_async_linear_transfer")))) {
        async_path = 1;
        submit_start = transfer_perf ? r_time() : 0;
        ret = virtio_gpu_transfer_to_host_3d_async(g, res, ctx_id, req->x,
                                                   req->y, req->w, req->h,
                                                   req->z, req->d,
                                                   req->level, req->offset,
                                                   cmd->stride,
                                                   cmd->layer_stride);
        if (transfer_perf)
            submit_ticks = r_time() - submit_start;
        ret = ret == 0 ? 0 : -EIO;
        goto out;
    }

    submit_start = transfer_perf ? r_time() : 0;
    if (linear_transfer_path &&
        virtio_gpu_cmdline_enabled("virtio_gpu_linear_transfer_mixed_wait")) {
        mixed_path = 1;
        ret = virtio_gpu_submit_mixed_async(
            g, cmd, sizeof(*cmd), NULL, 0, false, resp, sizeof(*resp),
            VIRTIO_GPU_RESP_OK_NODATA,
            transfer_perf ? &drain_sample : NULL);
        if (transfer_perf)
            command_ticks = r_time() - submit_start;
    } else {
        ret = virtio_gpu_submit_internal(g, cmd, sizeof(*cmd), NULL, 0, false,
                                         resp, sizeof(*resp),
                                         VIRTIO_GPU_RESP_OK_NODATA,
                                         transfer_perf ? &drain_ticks : NULL,
                                         transfer_perf ? &command_ticks : NULL,
                                         transfer_perf ? &drain_sample : NULL);
    }
    if (transfer_perf)
        submit_ticks = r_time() - submit_start;
    ret = ret == 0 ? 0 : -EIO;
out:
    if (transfer_perf)
        virtio_gpu_transfer_perf_log(
            ret, from_host, async_path, mixed_path, bound_scanout_path,
            wait_holder,
            r_time() - total_start, lock_acquired - total_start, submit_ticks,
            drain_ticks, command_ticks, drain_sample.submit_3d,
            drain_sample.flush, drain_sample.transfer, drain_sample.other,
            log_x, log_y, log_w, log_h, log_resource_id, log_ctx_id,
            log_res_width, log_res_height, log_target, log_bind, log_offset,
            log_stride, owner_tgid);
    virtio_gpu_op_unlock(g);
    return ret;
}

static int virtio_gpu_smoke_resource(struct virtio_gpu *g)
{
    struct virtio_gpu_resource *res;
    int scanout_bound = 0;

    if (virtio_gpu_resource_create_2d(g, VIRTIO_GPU_SMOKE_WIDTH,
                                      VIRTIO_GPU_SMOKE_HEIGHT,
                                      VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                      &res) != 0)
        return -1;

    uint32 *pixels = (uint32 *)res->backing;

    for (uint32 y = 0; y < VIRTIO_GPU_SMOKE_HEIGHT; y++) {
        for (uint32 x = 0; x < VIRTIO_GPU_SMOKE_WIDTH; x++) {
            uint8 r = (x * 255) / (VIRTIO_GPU_SMOKE_WIDTH - 1);
            uint8 gch = (y * 255) / (VIRTIO_GPU_SMOKE_HEIGHT - 1);
            pixels[y * VIRTIO_GPU_SMOKE_WIDTH + x] =
                0xff000000u | ((uint32)r << 16) | ((uint32)gch << 8) | 0x40;
        }
    }

    if (virtio_gpu_resource_attach_backing(g, res) != 0)
        goto fail;
    if (virtio_gpu_set_scanout(g, 0, res, 0, 0, VIRTIO_GPU_SMOKE_WIDTH,
                               VIRTIO_GPU_SMOKE_HEIGHT) != 0)
        goto fail;
    scanout_bound = 1;
    if (virtio_gpu_resource_transfer_2d(g, res, 0, 0,
                                        VIRTIO_GPU_SMOKE_WIDTH,
                                        VIRTIO_GPU_SMOKE_HEIGHT) != 0)
        goto fail;
    if (virtio_gpu_resource_flush(g, res, 0, 0, VIRTIO_GPU_SMOKE_WIDTH,
                                  VIRTIO_GPU_SMOKE_HEIGHT) != 0)
        goto fail;
    if (virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0) != 0)
        goto fail;
    scanout_bound = 0;

    printf("virtio_gpu: resource smoke ok resource=%u size=%ux%u bytes=%u\n",
           res->id, VIRTIO_GPU_SMOKE_WIDTH, VIRTIO_GPU_SMOKE_HEIGHT,
           res->backing_len);
    return virtio_gpu_resource_unref(g, res);

fail:
    if (scanout_bound)
        virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0);
    virtio_gpu_resource_unref(g, res);
    return -1;
}

static void virtio_gpu_fill_scanout_pattern(struct virtio_gpu_resource *res)
{
    uint32 *pixels = (uint32 *)res->backing;

    for (uint32 y = 0; y < res->height; y++) {
        for (uint32 x = 0; x < res->width; x++) {
            uint8 r = (x * 160) / (res->width ? res->width : 1);
            uint8 g = (y * 160) / (res->height ? res->height : 1);
            uint8 b = ((x ^ y) & 0x3f) + 0x30;
            pixels[y * res->width + x] =
                0xff000000u | ((uint32)r << 16) | ((uint32)g << 8) | b;
        }
    }
}

static int virtio_gpu_cmdline_video(uint32 *width, uint32 *height)
{
    char vbuf[32];
    uint32 w = 0;
    uint32 h = 0;
    const char *p;

    if (cmdline_get_param("video", vbuf, sizeof(vbuf)) != 0)
        return -1;

    p = vbuf;
    while (*p >= '0' && *p <= '9')
        w = w * 10 + (uint32)(*p++ - '0');
    if (*p != 'x' && *p != 'X')
        return -1;
    p++;
    while (*p >= '0' && *p <= '9')
        h = h * 10 + (uint32)(*p++ - '0');

    if (w < 640 || w > 2560 || h < 400 || h > 1600)
        return -1;
    *width = w;
    *height = h;
    return 0;
}

static int virtio_gpu_init_persistent_scanout(struct virtio_gpu *g)
{
    struct virtio_gpu_resource *res;
    uint32 width = g->scanout_width ? g->scanout_width : 640;
    uint32 height = g->scanout_height ? g->scanout_height : 480;
    uint32 reported_width = g->scanout_width;
    uint32 reported_height = g->scanout_height;
    uint32 fb_w = 0, fb_h = 0;
    uint32 video_w = 0, video_h = 0;
    int scanout_bound = 0;

    /*
     * Prefer an explicit kernel video= mode, then the firmware mode requested
     * by QEMU/OVMF, then the current virtio scanout size.  The reported
     * scanout can start as QEMU's default window geometry under GTK/GL, which
     * makes the desktop look tiny even though the boot framebuffer already
     * describes the intended guest resolution.
     */
    if (virtio_gpu_cmdline_video(&video_w, &video_h) == 0) {
        width = video_w;
        height = video_h;
        g->scanout_width = width;
        g->scanout_height = height;
        if (reported_width != width || reported_height != height) {
            printf("virtio_gpu: using cmdline video mode %ux%u for scanout (device reported %ux%u)\n",
                   width, height, reported_width, reported_height);
        }
    } else {
        fb_get_resolution(&fb_w, &fb_h);
        if (fb_w >= 640 && fb_h >= 400) {
            width = fb_w;
            height = fb_h;
            g->scanout_width = width;
            g->scanout_height = height;
            if (reported_width != width || reported_height != height) {
                printf("virtio_gpu: using fb0 mode %ux%u for scanout (device reported %ux%u)\n",
                       width, height, reported_width, reported_height);
            }
        } else if (reported_width >= 640 && reported_height >= 400) {
            width = reported_width;
            height = reported_height;
        }
    }

    if (g->scanout_width == 0 || g->scanout_height == 0) {
        printf("virtio_gpu: using default mode %ux%u for scanout fallback\n",
               width, height);
    }

    if (virtio_gpu_use_3d_scanout(g) &&
        virtio_gpu_resource_create_3d_backing(
            g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
            VIRTIO_GPU_PIPE_BIND_RENDER_TARGET |
            VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW |
            VIRTIO_GPU_PIPE_BIND_DISPLAY_TARGET |
            VIRTIO_GPU_PIPE_BIND_SCANOUT |
            VIRTIO_GPU_PIPE_BIND_SHARED |
            VIRTIO_GPU_PIPE_BIND_LINEAR,
            &res) == 0) {
        printf("virtio_gpu: using Alpine-style virgl 3D scanout resource\n");
    } else if (virtio_gpu_resource_create_2d(
                   g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                   &res) != 0) {
        return -1;
    }

    virtio_gpu_fill_scanout_pattern(res);

    if (virtio_gpu_resource_attach_backing(g, res) != 0)
        goto fail;
    if (virtio_gpu_set_scanout(g, 0, res, 0, 0, width, height) != 0)
        goto fail;
    scanout_bound = 1;
    g->bound_scanout_resource_id = res->id;
    if (virtio_gpu_resource_transfer_scanout(g, res, 0, 0, width, height) != 0)
        goto fail;
    if (virtio_gpu_resource_flush(g, res, 0, 0, width, height) != 0)
        goto fail;

    g->scanout_resource = res;
    printf("virtio_gpu: persistent scanout resource=%u kind=%s size=%ux%u bytes=%u alloc=%u\n",
           res->id, res->is_3d ? "3d" : "2d", width, height,
           res->backing_len, res->alloc_len);
    if (fb_init_virtio_gpu_scanout_backing(width, height, res->backing,
                                           res->backing_len,
                                           res->width * sizeof(uint32)) != 0)
        printf("virtio_gpu: warning: failed to expose scanout as /dev/fb0\n");
    return 0;

fail:
    if (scanout_bound)
        virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0);
    virtio_gpu_resource_unref(g, res);
    return -1;
}

/*
 * Hardware cursor plane.
 *
 * virtio-gpu exposes a dedicated cursor virtqueue (queue index 1) for
 * UPDATE_CURSOR / MOVE_CURSOR.  These commands never touch the control queue
 * that carries scanout/flush/3D work, so moving the pointer does not
 * recomposite or serialise behind the compositor's present.  This mirrors the
 * Weston DRM-backend hardware cursor plane on the same host.
 *
 * Cursor-queue commands take no device response: a single device-readable
 * descriptor is posted and the device returns it to the used ring.  We keep a
 * small ring of command buffers and reclaim consumed descriptors lazily.
 */
static void virtio_gpu_notify_cursor(struct virtio_gpu *g)
{
    volatile uint16 *notify_addr = (volatile uint16 *)
        ((uint8 *)g->pci.notify_base +
         g->cursorq.notify_off * g->pci.notify_off_multiplier);
    *notify_addr = 1;
}

static void virtio_gpu_cursor_post(struct virtio_gpu *g,
                                   struct virtio_gpu_update_cursor *cmd)
{
    struct virtio_gpu_queue *q = &g->cursorq;
    struct virtio_gpu_update_cursor *slotcmd;
    static int drop_logs;
    uint16 outstanding;
    uint16 slot;

    if (!g->cursor_ready)
        return;

    int intena = spin_lock_irqsave(&q->lock);
    /* Reclaim descriptors the device has already consumed. */
    q->used_idx = q->used->idx;
    outstanding = (uint16)(q->avail->idx - q->used_idx);
    if (outstanding >= q->size) {
        /*
         * Cursor motion is naturally coalesced by position.  Dropping a late
         * command is much safer than reusing a command slot that QEMU has not
         * consumed yet, which can corrupt an in-flight UPDATE_CURSOR image.
         */
        if (drop_logs < 4) {
            printf("virtio_gpu: cursor queue full, dropping cmd type=0x%x outstanding=%u size=%u\n",
                   cmd->hdr.type, outstanding, q->size);
            drop_logs++;
        }
        spin_unlock_irqrestore(&q->lock, intena);
        return;
    }
    slot = g->cursor_cmd_idx % g->cursor_ring;
    slotcmd = &((struct virtio_gpu_update_cursor *)g->cursor_cmd_page)[slot];
    *slotcmd = *cmd;

    q->desc[slot].addr = (uint64)slotcmd;
    q->desc[slot].len = sizeof(*slotcmd);
    q->desc[slot].flags = 0; /* device-read-only, no response */
    q->desc[slot].next = 0;

    q->avail->ring[q->avail->idx % q->size] = slot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g->cursor_cmd_idx++;
    virtio_gpu_notify_cursor(g);
    spin_unlock_irqrestore(&q->lock, intena);
}

static int virtio_gpu_cursor_queue_init(struct virtio_gpu *g)
{
    volatile struct virtio_pci_common_cfg *cfg = g->pci.common_cfg;
    struct virtio_gpu_queue *q = &g->cursorq;
    uint16 max;
    uint16 qsize;

    if (cfg->num_queues < 2) {
        printf("virtio_gpu: no cursor queue (num_queues=%u)\n",
               cfg->num_queues);
        return -1;
    }

    cfg->queue_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    max = cfg->queue_size;
    if (max == 0) {
        printf("virtio_gpu: cursor queue missing\n");
        return -1;
    }
    qsize = NUM;
    if (max < NUM)
        qsize = max;
    if (qsize < 2) {
        printf("virtio_gpu: cursor queue too small (%u)\n", qsize);
        return -1;
    }
    /* The command ring fits in one page; cap it so the cmd_page never
     * overflows even if the device advertised a large queue. */
    if (qsize > VIRTIO_GPU_CURSOR_RING)
        qsize = VIRTIO_GPU_CURSOR_RING;

    q->desc = kalloc();
    q->avail = kalloc();
    q->used = kalloc();
    g->cursor_cmd_page = kalloc();
    if (!q->desc || !q->avail || !q->used || !g->cursor_cmd_page) {
        if (q->desc) kfree(q->desc);
        if (q->avail) kfree(q->avail);
        if (q->used) kfree(q->used);
        if (g->cursor_cmd_page) kfree(g->cursor_cmd_page);
        q->desc = NULL;
        q->avail = NULL;
        q->used = NULL;
        g->cursor_cmd_page = NULL;
        printf("virtio_gpu: cursor queue kalloc failed\n");
        return -1;
    }
    memset(q->desc, 0, PGSIZE);
    memset(q->avail, 0, PGSIZE);
    memset(q->used, 0, PGSIZE);
    memset(g->cursor_cmd_page, 0, PGSIZE);

    cfg->queue_size = qsize;
    cfg->queue_desc = (uint64)q->desc;
    cfg->queue_driver = (uint64)q->avail;
    cfg->queue_device = (uint64)q->used;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->queue_enable = 1;
    q->size = qsize;
    q->used_idx = 0;
    q->notify_off = cfg->queue_notify_off;
    spin_init(&q->lock, "virtio_gpucur");
    g->cursor_ring = qsize;
    g->cursor_visible = 1;
    g->cursor_ready = 1;
    printf("virtio_gpu: cursor queue init size=%u notify_off=%u\n",
           q->size, q->notify_off);
    return 0;
}

/*
 * Upload (or replace) the hardware cursor image.  pixels points to a kernel
 * buffer of width*height BGRA pixels (0xAARRGGBB) with width,height <= 64.
 * Runs rarely (cursor theme change), so the control-queue create/transfer is
 * acceptable here.
 */
int virtio_gpu_user_set_cursor(const void *pixels, uint32 width,
                               uint32 height, uint32 hot_x, uint32 hot_y)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_update_cursor cmd;
    uint32 *dst;

    if (!g->initialized || !g->cursor_ready || pixels == NULL)
        return -ENODEV;
    if (width == 0 || height == 0 ||
        width > VIRTIO_GPU_CURSOR_DIM || height > VIRTIO_GPU_CURSOR_DIM)
        return -EINVAL;

    mutex_lock(&g->op_lock);
    if (g->cursor_resource == NULL) {
        struct virtio_gpu_resource *res = NULL;

        if (virtio_gpu_resource_create_2d(g, VIRTIO_GPU_CURSOR_DIM,
                                          VIRTIO_GPU_CURSOR_DIM,
                                          VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                          &res) != 0 || res == NULL) {
            mutex_unlock(&g->op_lock);
            return -EIO;
        }
        if (virtio_gpu_resource_attach_backing(g, res) != 0) {
            virtio_gpu_resource_unref(g, res);
            mutex_unlock(&g->op_lock);
            return -EIO;
        }
        g->cursor_resource = res;
        g->cursor_resource_id = res->id;
    }

    /* Compose the image into the 64x64 backing (rest transparent). */
    dst = (uint32 *)g->cursor_resource->backing;
    memset(dst, 0,
           VIRTIO_GPU_CURSOR_DIM * VIRTIO_GPU_CURSOR_DIM * sizeof(uint32));
    for (uint32 row = 0; row < height; row++)
        memmove(&dst[row * VIRTIO_GPU_CURSOR_DIM],
                (const uint32 *)pixels + (uint64)row * width,
                (uint64)width * sizeof(uint32));

    if (virtio_gpu_resource_transfer_2d(g, g->cursor_resource, 0, 0,
                                        VIRTIO_GPU_CURSOR_DIM,
                                        VIRTIO_GPU_CURSOR_DIM) != 0) {
        mutex_unlock(&g->op_lock);
        return -EIO;
    }

    g->cursor_hot_x = hot_x;
    g->cursor_hot_y = hot_y;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd.pos.scanout_id = 0;
    cmd.pos.x = (uint32)(g->cursor_x < 0 ? 0 : g->cursor_x);
    cmd.pos.y = (uint32)(g->cursor_y < 0 ? 0 : g->cursor_y);
    cmd.resource_id = g->cursor_resource_id;
    cmd.hot_x = hot_x;
    cmd.hot_y = hot_y;
    g->cursor_visible = 1;
    virtio_gpu_cursor_post(g, &cmd);
    mutex_unlock(&g->op_lock);
    return 0;
}

/*
 * Move (or show/hide) the hardware cursor.  Posts only to the cursor queue and
 * deliberately does NOT take the control-queue op_lock, so pointer motion is
 * never serialised behind a compositor present.
 */
int virtio_gpu_user_move_cursor(int32 x, int32 y, int visible)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_update_cursor cmd;

    if (!g->initialized || !g->cursor_ready)
        return -ENODEV;

    memset(&cmd, 0, sizeof(cmd));
    cmd.pos.scanout_id = 0;
    cmd.pos.x = (uint32)(x < 0 ? 0 : x);
    cmd.pos.y = (uint32)(y < 0 ? 0 : y);
    if (!visible) {
        /* resource_id 0 hides the cursor. */
        cmd.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
        cmd.resource_id = 0;
    } else if (!g->cursor_visible && g->cursor_resource_id) {
        /* Re-show after a hide: UPDATE_CURSOR rebinds the image resource. */
        cmd.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
        cmd.resource_id = g->cursor_resource_id;
        cmd.hot_x = g->cursor_hot_x;
        cmd.hot_y = g->cursor_hot_y;
    } else {
        cmd.hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
    }
    g->cursor_x = x;
    g->cursor_y = y;
    g->cursor_visible = visible ? 1 : 0;
    virtio_gpu_cursor_post(g, &cmd);
    return 0;
}

static int virtio_gpu_queue_init(struct virtio_gpu *g)
{
    volatile struct virtio_pci_common_cfg *cfg = g->pci.common_cfg;
    struct virtio_gpu_queue *q = &g->ctrlq;

    printf("virtio_gpu: queue init begin\n");
    cfg->queue_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint16 max = cfg->queue_size;
    if (max == 0) {
        printf("virtio_gpu: control queue missing\n");
        return -1;
    }
    uint16 qsize = NUM;
    if (max < NUM) {
        qsize = max;
        printf("virtio_gpu: control queue max=%d, using %d descriptor slots\n",
               max, max);
    }

    q->desc = kalloc();
    q->avail = kalloc();
    q->used = kalloc();
    g->cmd_page = kalloc();
    g->resp_page = kalloc();
    g->data_page = kalloc();
    if (!q->desc || !q->avail || !q->used || !g->cmd_page || !g->resp_page ||
        !g->data_page)
        panic("virtio_gpu: kalloc");

    memset(q->desc, 0, PGSIZE);
    memset(q->avail, 0, PGSIZE);
    memset(q->used, 0, PGSIZE);
    memset(g->cmd_page, 0, PGSIZE);
    memset(g->resp_page, 0, PGSIZE);
    memset(g->data_page, 0, PGSIZE);

    cfg->queue_size = qsize;
    cfg->queue_desc = (uint64)q->desc;
    cfg->queue_driver = (uint64)q->avail;
    cfg->queue_device = (uint64)q->used;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    cfg->queue_enable = 1;
    q->size = qsize;
    q->notify_off = cfg->queue_notify_off;
    spin_init(&q->lock, "virtio_gpuq");
    printf("virtio_gpu: queue init done size=%u notify_off=%u\n",
           q->size, q->notify_off);
    return 0;
}

void virtio_gpu_init(void)
{
    struct virtio_pci_discovery *vd = pci_get_virtio_gpu(0);
    struct virtio_gpu *g = &gpu;

    if (!vd || !vd->found)
        return;
    if (g->initialized)
        return;

    printf("virtio_gpu: init begin\n");
    if (!vd->common_cfg_cap || !vd->notify_cfg_cap || !vd->isr_cfg_cap ||
        !vd->device_cfg_cap) {
        printf("virtio_gpu: missing PCI capability, skipping driver init\n");
        return;
    }

    uint8 ccap = vd->common_cfg_cap;
    uint8 ncap = vd->notify_cfg_cap;
    uint8 icap = vd->isr_cfg_cap;
    uint8 dcap = vd->device_cfg_cap;

    uint8 cc_bar = pci_config_read8(vd->bus, vd->dev, vd->func, ccap + 4);
    uint32 cc_off = pci_config_read32(vd->bus, vd->dev, vd->func, ccap + 8);
    uint32 cc_len = pci_config_read32(vd->bus, vd->dev, vd->func, ccap + 12);
    uint8 n_bar = pci_config_read8(vd->bus, vd->dev, vd->func, ncap + 4);
    uint32 n_off = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 8);
    uint32 n_len = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 12);
    uint32 n_mult = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 16);
    uint8 i_bar = pci_config_read8(vd->bus, vd->dev, vd->func, icap + 4);
    uint32 i_off = pci_config_read32(vd->bus, vd->dev, vd->func, icap + 8);
    uint32 i_len = pci_config_read32(vd->bus, vd->dev, vd->func, icap + 12);
    uint8 d_bar = pci_config_read8(vd->bus, vd->dev, vd->func, dcap + 4);
    uint32 d_off = pci_config_read32(vd->bus, vd->dev, vd->func, dcap + 8);
    uint32 d_len = pci_config_read32(vd->bus, vd->dev, vd->func, dcap + 12);

    uint64 cfg_va = virtio_gpu_map_mmio_window(
        virtio_gpu_bar_base(vd, cc_bar), cc_off, cc_len);
    uint64 notify_va = virtio_gpu_map_mmio_window(
        virtio_gpu_bar_base(vd, n_bar), n_off, n_len);
    uint64 isr_va = virtio_gpu_map_mmio_window(
        virtio_gpu_bar_base(vd, i_bar), i_off, i_len);
    uint64 dev_cfg_va = virtio_gpu_map_mmio_window(
        virtio_gpu_bar_base(vd, d_bar), d_off, d_len);

    memset(g, 0, sizeof(*g));
    spin_init(&g->lock, "virtio_gpu");
    mutex_init(&g->op_lock, "virtio_gpuop");
    g->next_resource_id = 1;
    g->next_context_id = 2;
    g->pci.use_pci = 1;
    g->pci.common_cfg = (volatile struct virtio_pci_common_cfg *)cfg_va;
    g->pci.notify_base = (volatile uint16 *)notify_va;
    g->pci.notify_off_multiplier = n_mult;
    g->pci.isr = (volatile uint8 *)isr_va;
    g->config = (volatile struct virtio_gpu_config *)dev_cfg_va;
    g->next_fence_id = 1;

    volatile struct virtio_pci_common_cfg *cfg = g->pci.common_cfg;

    printf("virtio_gpu: reset device\n");
    cfg->device_status = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int i = 0; cfg->device_status != 0 && i < 1000000; i++)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (cfg->device_status != 0) {
        printf("virtio_gpu: reset timed out status=0x%x\n",
               cfg->device_status);
        return;
    }

    printf("virtio_gpu: negotiate features\n");
    uint8 status = VIRTIO_CONFIG_S_ACKNOWLEDGE;
    cfg->device_status = status;
    status |= VIRTIO_CONFIG_S_DRIVER;
    cfg->device_status = status;

    cfg->device_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32 features0 = cfg->device_feature;
    uint32 driver_features0 = 0;
    if (features0 & (1u << VIRTIO_GPU_F_VIRGL))
        driver_features0 |= (1u << VIRTIO_GPU_F_VIRGL);
    if (features0 & (1u << VIRTIO_GPU_F_EDID))
        driver_features0 |= (1u << VIRTIO_GPU_F_EDID);
    if (features0 & (1u << VIRTIO_GPU_F_CONTEXT_INIT))
        driver_features0 |= (1u << VIRTIO_GPU_F_CONTEXT_INIT);
    g->features0 = features0;
    g->driver_features0 = driver_features0;

    cfg->driver_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->driver_feature = driver_features0;

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(cfg->device_status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        printf("virtio_gpu: device rejected feature set\n");
        cfg->device_status = 0;
        return;
    }

    printf("virtio_gpu: setup queue\n");
    if (virtio_gpu_queue_init(g) != 0) {
        cfg->device_status = 0;
        return;
    }
    /* Optional dedicated cursor queue; failure leaves cursor_ready=0 and the
     * compositor falls back to the software cursor. */
    (void)virtio_gpu_cursor_queue_init(g);

    printf("virtio_gpu: driver ok\n");
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g->initialized = 1;

    struct irq_desc virtio_gpu_irq_desc = {
        .handler = virtio_gpu_intr,
        .data = g,
        .dev = NULL,
    };
    int irq_ret = register_irq_handler(PLIC_IRQ(vd->irq_line),
                                       &virtio_gpu_irq_desc);
    if (irq_ret == 0) {
        extern void plic_enable_irq_level(int irq);
        plic_enable_irq_level(vd->irq_line);
    } else {
        printf("virtio_gpu: WARNING: IRQ %d registration failed (%d), polling fallback only\n",
               vd->irq_line, irq_ret);
    }

    printf("virtio_gpu: initialized queues=%u features0=0x%x driver_features0=0x%x scanouts=%u capsets=%u irq=%d\n",
           cfg->num_queues, features0, driver_features0,
           g->config->num_scanouts, g->config->num_capsets, vd->irq_line);

    virtio_gpu_query_capsets(g);
    virtio_gpu_smoke_context(g);
    virtio_gpu_submit_display_info(g);
    virtio_gpu_get_edid_mode(g, NULL, NULL, NULL, 1);
    virtio_gpu_smoke_resource(g);
    virtio_gpu_init_persistent_scanout(g);
    if (virtio_gpu_has_virgl())
        fb_gpu_register_render_node();
}

void virtio_gpu_get_fb_stats(struct fb_gpu_stats *stats)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_stats vg_stats;
    uint64 async_pending;
    uint64 async_depth;

    if (!g->initialized)
        return;

    spin_lock(&g->lock);
    vg_stats = g->stats;
    async_pending = g->async_count;
    async_depth = g->async_depth ? (uint64)g->async_depth : 2;
    spin_unlock(&g->lock);

    stats->virtio_commands = vg_stats.commands;
    stats->virtio_failures = vg_stats.failures;
    stats->virtio_timeouts = vg_stats.timeouts;
    stats->virtio_resources = vg_stats.resources;
    stats->virtio_resource_bytes = vg_stats.resource_bytes;
    stats->virtio_transfers = vg_stats.transfers;
    stats->virtio_flushes = vg_stats.flushes;
    stats->virtio_scanouts = vg_stats.scanouts;
    stats->virtio_capsets = vg_stats.capsets;
    stats->virtio_virgl = vg_stats.virgl;
    stats->virtio_virgl_version = vg_stats.virgl_version;
    stats->virtio_virgl_size = vg_stats.virgl_size;
    stats->virtio_contexts = vg_stats.contexts;
    stats->virtio_context_failed = vg_stats.context_failed;
    stats->virtio_context_failures = vg_stats.context_failures;
    stats->virtio_submits = vg_stats.submits;
    stats->virtio_fences = vg_stats.fences;
    stats->virtio_last_fence = vg_stats.last_fence;
    stats->virtio_irq_completions = vg_stats.irq_completions;
    stats->virtio_poll_fallbacks = vg_stats.poll_fallbacks;
    stats->virtio_async_posted = vg_stats.async_posted;
    stats->virtio_async_posted_submit_3d = vg_stats.async_posted_submit_3d;
    stats->virtio_async_posted_flush = vg_stats.async_posted_flush;
    stats->virtio_async_posted_transfer = vg_stats.async_posted_transfer;
    stats->virtio_async_retired = vg_stats.async_retired;
    stats->virtio_async_pending = async_pending;
    stats->virtio_async_depth = async_depth;
    stats->virtio_async_make_room_calls = vg_stats.async_make_room_calls;
    stats->virtio_async_make_room_submit_3d_calls =
        vg_stats.async_make_room_submit_3d_calls;
    stats->virtio_async_make_room_flush_calls =
        vg_stats.async_make_room_flush_calls;
    stats->virtio_async_make_room_transfer_calls =
        vg_stats.async_make_room_transfer_calls;
    stats->virtio_async_make_room_stalls = vg_stats.async_make_room_stalls;
    stats->virtio_async_make_room_submit_3d_stalls =
        vg_stats.async_make_room_submit_3d_stalls;
    stats->virtio_async_make_room_flush_stalls =
        vg_stats.async_make_room_flush_stalls;
    stats->virtio_async_make_room_transfer_stalls =
        vg_stats.async_make_room_transfer_stalls;
    stats->virtio_async_wait_progress_calls =
        vg_stats.async_wait_progress_calls;
    stats->virtio_async_make_room_wait_ticks =
        vg_stats.async_make_room_wait_ticks;
    stats->virtio_async_make_room_last_wait_us =
        vg_stats.async_make_room_last_wait_us;
    stats->virtio_async_make_room_max_wait_us =
        vg_stats.async_make_room_max_wait_us;
}

int virtio_gpu_has_virgl(void)
{
    struct virtio_gpu *g = &gpu;

    return g->initialized && g->virgl_capset_id &&
        (g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL));
}

int virtio_gpu_probe_scanout(uint32 *width, uint32 *height)
{
    struct virtio_gpu *g = &gpu;
    int ret;

    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PROBE);
    ret = virtio_gpu_get_display_info(g, width, height, 0);
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_probe_edid_mode(uint32 *width, uint32 *height,
                               uint32 *refresh_millihz)
{
    struct virtio_gpu *g = &gpu;
    int ret;

    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    if (refresh_millihz != NULL)
        *refresh_millihz = 0;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PROBE);
    ret = virtio_gpu_get_edid_mode(g, width, height, refresh_millihz, 0);
    if (ret != 0 && g->edid_width != 0 && g->edid_height != 0) {
        if (width != NULL)
            *width = g->edid_width;
        if (height != NULL)
            *height = g->edid_height;
        if (refresh_millihz != NULL)
            *refresh_millihz = g->edid_refresh_millihz;
        ret = 0;
    }
    virtio_gpu_op_unlock(g);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_resize_scanout(uint32 width, uint32 height)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    struct virtio_gpu_resource *old;
    int ret = -EIO;
    int bound = 0;

    if (width < 640 || width > 2560 || height < 400 || height > 1600)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_RESIZE);
    old = g->scanout_resource;
    if (old != NULL && old->width == width && old->height == height) {
        ret = 0;
        goto out;
    }

    if (virtio_gpu_use_3d_scanout(g) &&
        virtio_gpu_resource_create_3d_backing(
            g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
            VIRTIO_GPU_PIPE_BIND_RENDER_TARGET |
            VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW |
            VIRTIO_GPU_PIPE_BIND_DISPLAY_TARGET |
            VIRTIO_GPU_PIPE_BIND_SCANOUT |
            VIRTIO_GPU_PIPE_BIND_SHARED |
            VIRTIO_GPU_PIPE_BIND_LINEAR,
            &res) == 0) {
        printf("virtio_gpu: using Alpine-style virgl 3D scanout resource for mode set\n");
    } else if (virtio_gpu_resource_create_2d(
                   g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                   &res) != 0) {
        goto out;
    }
    memset(res->backing, 0, res->backing_len);

    if (virtio_gpu_resource_attach_backing(g, res) != 0)
        goto fail_new;
    if (virtio_gpu_set_scanout(g, 0, res, 0, 0, width, height) != 0)
        goto fail_new;
    bound = 1;
    g->bound_scanout_resource_id = res->id;
    if (virtio_gpu_resource_transfer_scanout(g, res, 0, 0, width, height) != 0)
        goto fail_new;
    if (virtio_gpu_resource_flush(g, res, 0, 0, width, height) != 0)
        goto fail_new;

    g->scanout_resource = res;
    g->scanout_width = width;
    g->scanout_height = height;
    virtio_gpu_drop_present_flip_resources(g);
    virtio_gpu_page_flip_scanout_set_reset(g);
    g->present_scanout_ctx_id = 0;
    g->present_scanout_resource_id = 0;
    g->bound_scanout_resource_id = res->id;
    ret = fb_replace_virtio_gpu_scanout_backing(width, height, res->backing,
                                                res->backing_len,
                                                res->width * sizeof(uint32));
    if (ret != 0) {
        g->scanout_resource = old;
        goto fail_new;
    }

    /*
     * Existing userspace may still have the old direct scanout mmap.  Keep the
     * old resource alive instead of freeing pages that might still be mapped;
     * the compositor remaps the current scanout after FBIOPUT succeeds.
     */
    ret = 0;
    goto out;

fail_new:
    if (bound) {
        if (old != NULL) {
            virtio_gpu_set_scanout(g, 0, old, 0, 0, old->width, old->height);
            g->bound_scanout_resource_id = old->id;
        } else {
            virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0);
            g->bound_scanout_resource_id = 0;
        }
    }
    virtio_gpu_resource_unref(g, res);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

int virtio_gpu_bind_resource_scanout(uint32 resource_id, uint32 x, uint32 y,
                                     uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    uint32 scanout_w;
    uint32 scanout_h;
    int already_bound = 0;
    int full_size_resource;
    int ret = -EIO;
    int scanout_perf;
    int scanout_path = 0;
    int prepared_async_flush = 0;
    int wait_holder = VIRTIO_GPU_OP_NONE;
    struct virtio_gpu_async_submit scanout_flush_prep;
    uint64 total_start = 0;
    uint64 lock_acquired = 0;
    uint64 set_scanout_start;
    uint64 set_scanout_ticks = 0;
    uint64 flush_start;
    uint64 flush_ticks = 0;

    if (resource_id == 0 || w == 0 || h == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    memset(&scanout_flush_prep, 0, sizeof(scanout_flush_prep));
    if (virtio_gpu_async_scanout_flush_enabled() &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_no_scanout_flush") &&
        __atomic_load_n(&g->bound_scanout_resource_id,
                        __ATOMIC_RELAXED) == resource_id &&
        virtio_gpu_resource_flush_async_prepare(resource_id, x, y, w, h,
                                                &scanout_flush_prep) == 0)
        prepared_async_flush = 1;

    scanout_perf = virtio_gpu_cmdline_enabled("virtio_gpu_scanout_perf");
    if (scanout_perf)
        total_start = r_time();
    if (scanout_perf && mutex_trylock(&g->op_lock)) {
        wait_holder = VIRTIO_GPU_OP_NONE;
    } else {
        if (scanout_perf)
            wait_holder = __atomic_load_n(&g->op_lock_holder,
                                          __ATOMIC_RELAXED);
        mutex_lock(&g->op_lock);
    }
    __atomic_store_n(&g->op_lock_holder, VIRTIO_GPU_OP_SCANOUT,
                     __ATOMIC_RELAXED);
    if (scanout_perf)
        lock_acquired = r_time();
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    if (res == NULL || !res->attached ||
        x > res->width || w > res->width - x ||
        y > res->height || h > res->height - y) {
        spin_unlock(&g->lock);
        ret = -EINVAL;
        goto out;
    }
    spin_unlock(&g->lock);

    /*
     * SET_SCANOUT changes the host-visible mode to the supplied rectangle.
     * Linux's fast path is safe when KMS page-flips a full-size framebuffer;
     * binding a window-sized virgl resource here makes QEMU resize between the
     * desktop and the client window.  Keep partial scanout behind an explicit
     * diagnostic flag so normal windowed compositing cannot trigger that jump.
     */
    scanout_w = g->scanout_width;
    scanout_h = g->scanout_height;
    already_bound = g->bound_scanout_resource_id == res->id;
    full_size_resource = res->width == scanout_w && res->height == scanout_h;
    if (already_bound &&
        ((uint64)x + w > scanout_w || (uint64)y + h > scanout_h)) {
        ret = -EINVAL;
        goto out;
    }
    if (!virtio_gpu_cmdline_enabled("virtio_gpu_allow_partial_scanout") &&
        !already_bound && !full_size_resource &&
        (w != scanout_w || h != scanout_h)) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    if (!already_bound) {
        uint32 bind_x = full_size_resource ? 0 : x;
        uint32 bind_y = full_size_resource ? 0 : y;
        uint32 bind_w = full_size_resource ? scanout_w : w;
        uint32 bind_h = full_size_resource ? scanout_h : h;

        set_scanout_start = scanout_perf ? r_time() : 0;
        if (virtio_gpu_set_scanout(g, 0, res, bind_x, bind_y,
                                   bind_w, bind_h) != 0)
            goto out;
        if (scanout_perf)
            set_scanout_ticks += r_time() - set_scanout_start;
        g->bound_scanout_resource_id = res->id;
        if (!virtio_gpu_present_flip_slot_for_id(g, res->id, NULL)) {
            virtio_gpu_invalidate_present_flip_slot(g, 0);
            virtio_gpu_invalidate_present_flip_slot(g, 1);
        }
        virtio_gpu_page_flip_scanout_set_reset(g);
    }
    /*
     * The compositor's virgl target is updated by GL, not by the CPU backing.
     * A TRANSFER_TO_HOST here would overwrite that rendered target, but QEMU
     * still needs RESOURCE_FLUSH as the display update signal for the bound
     * scanout resource.  Leave a diagnostic escape hatch for comparing hosts
     * that repaint a 3D scanout without this notification.
     */
    flush_start = scanout_perf ? r_time() : 0;
    if (virtio_gpu_cmdline_enabled("virtio_gpu_no_scanout_flush")) {
        scanout_path = 1;
        ret = 0;
    } else if (already_bound && virtio_gpu_async_scanout_flush_enabled()) {
        scanout_path = 2;
        if (prepared_async_flush)
            ret = virtio_gpu_async_post_prepared(
                g, &scanout_flush_prep,
                VIRTIO_GPU_ASYNC_REASON_FLUSH) == 0 ? 0 : -EIO;
        else
            ret = virtio_gpu_resource_flush_async(g, res, x, y, w, h) == 0 ?
                  0 : -EIO;
    } else {
        scanout_path = 3;
        ret = virtio_gpu_resource_flush(g, res, x, y, w, h) == 0 ? 0 : -EIO;
    }
    if (scanout_perf)
        flush_ticks = r_time() - flush_start;
out:
    if (scanout_perf)
        virtio_gpu_scanout_perf_log(
            ret, scanout_path, already_bound, wait_holder,
            r_time() - total_start, lock_acquired - total_start,
            set_scanout_ticks, flush_ticks);
    virtio_gpu_op_unlock(g);
    if (prepared_async_flush)
        virtio_gpu_async_submit_free(&scanout_flush_prep);
    return ret;
}

int virtio_gpu_page_flip_resource(uint32 resource_id, uint32 w, uint32 h,
                                  uint32 *flags_out)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    uint32 scanout_w;
    uint32 scanout_h;
    int already_bound;
    int registered;
    int rebind = 0;
    int async_scanout = 0;
    int ret;
    struct virtio_gpu_async_submit scanout_prep;
    static int flip_logs;

    if (flags_out != NULL)
        *flags_out = 0;
    if (resource_id == 0 || w == 0 || h == 0)
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    memset(&scanout_prep, 0, sizeof(scanout_prep));
    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PAGE_FLIP);
    spin_lock(&g->lock);
    res = virtio_gpu_lookup_resource_locked(g, resource_id);
    scanout_w = g->scanout_width;
    scanout_h = g->scanout_height;
    if (res == NULL || !res->attached ||
        res->width != scanout_w || res->height != scanout_h ||
        w != scanout_w || h != scanout_h) {
        spin_unlock(&g->lock);
        ret = -EINVAL;
        goto out;
    }
    spin_unlock(&g->lock);

    if (g->page_flip_scanout_width != scanout_w ||
        g->page_flip_scanout_height != scanout_h)
        virtio_gpu_page_flip_scanout_set_reset(g);
    already_bound = g->bound_scanout_resource_id == res->id;
    registered = virtio_gpu_page_flip_scanout_set_contains(g, res->id);
    if (!already_bound && !registered) {
        if ((virtio_gpu_cmdline_enabled(
                 "virtio_gpu_async_page_flip_scanout") ||
             virtio_gpu_cmdline_enabled("vgpu_async_pf")) &&
            virtio_gpu_async_scanout_flush_enabled() &&
            virtio_gpu_set_scanout_async_prepare(
                0, res->id, 0, 0, scanout_w, scanout_h,
                &scanout_prep) == 0) {
            if (virtio_gpu_async_post_prepared(
                    g, &scanout_prep,
                    VIRTIO_GPU_ASYNC_REASON_OTHER) != 0) {
                virtio_gpu_async_submit_free(&scanout_prep);
                if (virtio_gpu_set_scanout(g, 0, res, 0, 0,
                                           scanout_w, scanout_h) != 0) {
                    ret = -EIO;
                    goto out;
                }
            } else {
                async_scanout = 1;
            }
        } else {
            if (virtio_gpu_set_scanout(g, 0, res, 0, 0,
                                       scanout_w, scanout_h) != 0) {
                ret = -EIO;
                goto out;
            }
        }
        g->bound_scanout_resource_id = res->id;
        g->scanout_resource = res;
        virtio_gpu_page_flip_scanout_set_note(g, res->id, scanout_w,
                                              scanout_h);
        rebind = 1;
        if (!virtio_gpu_present_flip_slot_for_id(g, res->id, NULL)) {
            virtio_gpu_invalidate_present_flip_slot(g, 0);
            virtio_gpu_invalidate_present_flip_slot(g, 1);
        }
    }
    if (virtio_gpu_async_scanout_flush_enabled())
        ret = virtio_gpu_resource_flush_async(g, res, 0, 0,
                                              scanout_w, scanout_h) == 0 ?
            0 : -EIO;
    else
        ret = virtio_gpu_resource_flush(g, res, 0, 0, scanout_w,
                                        scanout_h) == 0 ? 0 : -EIO;
    if (ret == 0 && flip_logs < 8) {
        printf("virtio_gpu: page-flip present resource=%u size=%ux%u already_bound=%d registered=%d rebind=%d set_count=%u async_scanout=%d\n",
               res->id, scanout_w, scanout_h, already_bound, registered,
               rebind, g->page_flip_scanout_set_count, async_scanout);
        flip_logs++;
    }
    if (ret == 0 && flags_out != NULL) {
        if (rebind)
            *flags_out |= FB_GPU_PAGE_FLIP_F_SCANOUT_REBIND;
        if (registered || rebind)
            *flags_out |= FB_GPU_PAGE_FLIP_F_SCANOUT_CACHED;
    }
out:
    if (scanout_prep.cmd != NULL || scanout_prep.resp != NULL ||
        scanout_prep.data != NULL)
        virtio_gpu_async_submit_free(&scanout_prep);
    virtio_gpu_op_unlock(g);
    return ret;
}

static int virtio_gpu_ensure_present_context(struct virtio_gpu *g,
                                             uint32 *ctx_id)
{
    struct virtio_gpu_context *ctx;
    struct virtio_gpu_capset *capset;
    uint32 id;
    int ret;

    spin_lock(&g->lock);
    if (g->present_ctx_id != 0) {
        ctx = virtio_gpu_lookup_context_locked(g, g->present_ctx_id);
        if (ctx != NULL && !ctx->failed) {
            *ctx_id = g->present_ctx_id;
            spin_unlock(&g->lock);
            return 0;
        }
        g->present_ctx_id = 0;
    }
    capset = virtio_gpu_lookup_capset_locked(g, g->virgl_capset_id);
    if (capset == NULL) {
        spin_unlock(&g->lock);
        return -ENODEV;
    }
    ctx = virtio_gpu_alloc_context_slot_locked(g);
    if (ctx == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        return -ENOSPC;
    }
    id = g->next_context_id++;
    if (g->next_context_id == 0)
        g->next_context_id = 2;
    memset(ctx, 0, sizeof(*ctx));
    ctx->in_use = 1;
    ctx->id = id;
    ctx->capset_id = capset->id;
    g->present_ctx_id = id;
    spin_unlock(&g->lock);

    ret = virtio_gpu_create_context(g, id, capset->id, "xv6-present-copy");
    if (ret != 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, id);
        if (ctx != NULL)
            memset(ctx, 0, sizeof(*ctx));
        if (g->present_ctx_id == id)
            g->present_ctx_id = 0;
        spin_unlock(&g->lock);
        return -EIO;
    }
    *ctx_id = id;
    return 0;
}

static void virtio_gpu_note_diagnostic_present_locked(
    struct virtio_gpu *g, uint32 ctx_id, uint32 resource_id,
    uint32 src_x, uint32 src_y, uint32 dst_x, uint32 dst_y,
    uint32 w, uint32 h)
{
    g->diagnostic_present_ctx_id = ctx_id;
    g->diagnostic_present_resource_id = resource_id;
    g->diagnostic_present_src_x = src_x;
    g->diagnostic_present_src_y = src_y;
    g->diagnostic_present_dst_x = dst_x;
    g->diagnostic_present_dst_y = dst_y;
    g->diagnostic_present_w = w;
    g->diagnostic_present_h = h;
}

static void virtio_gpu_drop_present_context(struct virtio_gpu *g, uint32 ctx_id)
{
    struct virtio_gpu_context *ctx;

    if (ctx_id == 0)
        return;
    (void)virtio_gpu_destroy_context(g, ctx_id);
    spin_lock(&g->lock);
    if (g->present_scanout_ctx_id == ctx_id) {
        g->present_scanout_ctx_id = 0;
        g->present_scanout_resource_id = 0;
    }
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx != NULL && ctx->owner_id == 0 && ctx->owner_tgid == 0) {
        if (ctx->failed && g->stats.context_failed > 0)
            g->stats.context_failed--;
        memset(ctx, 0, sizeof(*ctx));
    }
    if (g->present_ctx_id == ctx_id)
        g->present_ctx_id = 0;
    spin_unlock(&g->lock);
}

static int virtio_gpu_ensure_scanout_attached(struct virtio_gpu *g,
                                              uint32 ctx_id,
                                              uint32 resource_id)
{
    struct virtio_gpu_context *ctx;
    uint32 old_ctx = 0;
    uint32 old_res = 0;
    int cached;

    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    cached = ctx != NULL && !ctx->failed &&
             g->present_scanout_ctx_id == ctx_id &&
             g->present_scanout_resource_id == resource_id;
    if (cached) {
        spin_unlock(&g->lock);
        return 0;
    }
    if (g->present_scanout_ctx_id != 0) {
        old_ctx = g->present_scanout_ctx_id;
        old_res = g->present_scanout_resource_id;
        g->present_scanout_ctx_id = 0;
        g->present_scanout_resource_id = 0;
    }
    spin_unlock(&g->lock);

    if (old_ctx != 0 && old_res != 0)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, old_ctx, old_res);

    if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                    ctx_id, resource_id) != 0)
        return -EIO;

    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx != NULL && !ctx->failed) {
        g->present_scanout_ctx_id = ctx_id;
        g->present_scanout_resource_id = resource_id;
    }
    spin_unlock(&g->lock);
    return 0;
}

static void virtio_gpu_drop_present_flip_resources(struct virtio_gpu *g)
{
    for (int i = 0; i < 2; i++) {
        struct virtio_gpu_resource *res = g->present_flip_resource[i];

        g->present_flip_valid[i] = 0;
        g->present_flip_rect_x[i] = 0;
        g->present_flip_rect_y[i] = 0;
        g->present_flip_rect_w[i] = 0;
        g->present_flip_rect_h[i] = 0;
        if (res == NULL)
            continue;
        g->present_flip_resource[i] = NULL;
        if (g->bound_scanout_resource_id == res->id) {
            (void)virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0);
            g->bound_scanout_resource_id = 0;
        }
        (void)virtio_gpu_resource_unref(g, res);
    }
    g->present_flip_index = 0;
}

static int virtio_gpu_present_flip_slot_for_id(struct virtio_gpu *g,
                                               uint32 resource_id,
                                               uint32 *slot_out)
{
    if (resource_id == 0)
        return 0;
    for (uint32 i = 0; i < 2; i++) {
        struct virtio_gpu_resource *res = g->present_flip_resource[i];

        if (res != NULL && res->id == resource_id) {
            if (slot_out != NULL)
                *slot_out = i;
            return 1;
        }
    }
    return 0;
}

static void virtio_gpu_invalidate_present_flip_slot(struct virtio_gpu *g,
                                                    uint32 slot)
{
    if (slot >= 2)
        return;
    g->present_flip_valid[slot] = 0;
    g->present_flip_base_generation[slot] = 0;
    g->present_flip_rect_x[slot] = 0;
    g->present_flip_rect_y[slot] = 0;
    g->present_flip_rect_w[slot] = 0;
    g->present_flip_rect_h[slot] = 0;
}

static int virtio_gpu_ensure_present_flip_resource(
    struct virtio_gpu *g, uint32 width, uint32 height,
    struct virtio_gpu_resource **out, uint32 *slot_out)
{
    struct virtio_gpu_resource *res;
    uint32 first;

    if (out == NULL || width == 0 || height == 0 ||
        width > 4096 || height > 4096)
        return -EINVAL;

    first = (g->present_flip_index + 1) & 1u;
    for (int pass = 0; pass < 2; pass++) {
        uint32 slot = (first + (uint32)pass) & 1u;

        res = g->present_flip_resource[slot];
        if (res != NULL && res->width == width && res->height == height &&
            res->attached) {
            if (g->bound_scanout_resource_id == res->id)
                continue;
            *out = res;
            if (slot_out)
                *slot_out = slot;
            return 0;
        }
    }

    for (int pass = 0; pass < 2; pass++) {
        uint32 slot = (first + (uint32)pass) & 1u;

        res = g->present_flip_resource[slot];
        if (res != NULL) {
            if (g->bound_scanout_resource_id == res->id)
                continue;
            g->present_flip_resource[slot] = NULL;
            virtio_gpu_invalidate_present_flip_slot(g, slot);
            (void)virtio_gpu_resource_unref(g, res);
        }

        if (virtio_gpu_resource_create_3d_backing(
                g, width, height, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                VIRTIO_GPU_PIPE_BIND_RENDER_TARGET |
                VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW |
                VIRTIO_GPU_PIPE_BIND_DISPLAY_TARGET |
                VIRTIO_GPU_PIPE_BIND_SCANOUT |
                VIRTIO_GPU_PIPE_BIND_SHARED |
                VIRTIO_GPU_PIPE_BIND_LINEAR,
                &res) != 0)
            return -EIO;
        memset(res->backing, 0, res->backing_len);
        if (virtio_gpu_resource_attach_backing(g, res) != 0) {
            (void)virtio_gpu_resource_unref(g, res);
            return -EIO;
        }
        /*
         * Match the persistent scanout resource setup: make QEMU/virgl create
         * host storage from the newly attached guest backing before the
         * resource is used as a pageflip target.  Without this, copies into
         * the flip resource can complete but TRANSFER_FROM_HOST_3D validates
         * black on WSL/D3D12, causing the same blank/blinking path the Linux
         * KMS import sequence avoids.
         */
        if (virtio_gpu_resource_transfer_scanout(g, res, 0, 0, width,
                                                 height) != 0) {
            (void)virtio_gpu_resource_unref(g, res);
            return -EIO;
        }
        g->present_flip_resource[slot] = res;
        virtio_gpu_invalidate_present_flip_slot(g, slot);
        *out = res;
        if (slot_out)
            *slot_out = slot;
        return 0;
    }
    return -EAGAIN;
}

static uint32 virtio_gpu_resource_sample_pixel(struct virtio_gpu_resource *res,
                                               uint32 x, uint32 y)
{
    uint64 off;
    uint32 page_idx;
    uint32 page_off;
    uint8 *p;

    if (res == NULL || res->width == 0 ||
        res->height == 0 || x >= res->width || y >= res->height)
        return 0;
    off = ((uint64)y * res->width + x) * sizeof(uint32);
    if (res->backing != NULL && off + sizeof(uint32) <= res->backing_len)
        return *(uint32 *)((uint8 *)res->backing + off);
    if (res->pages == NULL)
        return 0;
    page_idx = off / PGSIZE;
    page_off = off & (PGSIZE - 1);
    if (page_idx >= res->npages || page_off + sizeof(uint32) > PGSIZE ||
        res->pages[page_idx] == NULL)
        return 0;
    p = (uint8 *)PA2VA(__page_to_pa(res->pages[page_idx])) + page_off;
    return *(uint32 *)p;
}

struct virtio_gpu_present_samples {
    uint32 p[VIRTIO_GPU_PRESENT_SAMPLE_COUNT];
    uint32 nonblack;
    uint32 unique_nonblack;
};

static void virtio_gpu_count_present_samples(
    struct virtio_gpu_present_samples *samples)
{
    samples->nonblack = 0;
    samples->unique_nonblack = 0;
    for (int i = 0; i < VIRTIO_GPU_PRESENT_SAMPLE_COUNT; i++) {
        uint32 rgb = samples->p[i] & 0x00ffffffu;
        int seen = 0;

        if (rgb == 0)
            continue;
        samples->nonblack++;
        for (int j = 0; j < i; j++) {
            if ((samples->p[j] & 0x00ffffffu) == rgb) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            samples->unique_nonblack++;
    }
}

static int virtio_gpu_resource_host_samples(
    struct virtio_gpu *g,
    struct virtio_gpu_resource *res,
    uint32 ctx_id,
    uint32 x, uint32 y,
    uint32 w, uint32 h,
    struct virtio_gpu_present_samples *samples)
{
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;

    if (res == NULL || res->width == 0 || res->height == 0 ||
        w == 0 || h == 0 ||
        x > res->width || w > res->width - x ||
        y > res->height || h > res->height - y ||
        (res->backing == NULL && (res->pages == NULL || res->npages == 0)) ||
        samples == NULL)
        return -EINVAL;

    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer->hdr.ctx_id = ctx_id;
    transfer->box.x = x;
    transfer->box.y = y;
    transfer->box.z = 0;
    transfer->box.w = w;
    transfer->box.h = h;
    transfer->box.d = 1;
    transfer->offset = ((uint64)y * res->width + x) * sizeof(uint32);
    transfer->resource_id = res->id;
    transfer->level = 0;
    transfer->stride = res->width * sizeof(uint32);
    transfer->layer_stride = (uint64)transfer->stride * res->height;
    if (virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                          resp, sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_NODATA) != 0)
        return -EIO;

    memset(samples, 0, sizeof(*samples));
    samples->p[0] = virtio_gpu_resource_sample_pixel(res, x + w / 2,
                                                     y + h / 2);
    samples->p[1] = virtio_gpu_resource_sample_pixel(res, x + w / 4,
                                                     y + h / 4);
    samples->p[2] = virtio_gpu_resource_sample_pixel(res, x + (w * 3) / 4,
                                                     y + h / 4);
    samples->p[3] = virtio_gpu_resource_sample_pixel(res, x + w / 4,
                                                     y + (h * 3) / 4);
    samples->p[4] = virtio_gpu_resource_sample_pixel(res,
                                                     x + (w * 3) / 4,
                                                     y + (h * 3) / 4);
    virtio_gpu_count_present_samples(samples);
    return 0;
}

static int virtio_gpu_resource_host_nonblack(struct virtio_gpu *g,
                                             struct virtio_gpu_resource *res,
                                             uint32 ctx_id,
                                             uint32 x, uint32 y,
                                             uint32 w, uint32 h)
{
    struct virtio_gpu_present_samples samples;
    int ret = virtio_gpu_resource_host_samples(g, res, ctx_id, x, y, w, h,
                                               &samples);

    return ret == 0 ? (int)samples.nonblack : ret;
}

static int virtio_gpu_present_sample_matches(
    const struct virtio_gpu_present_samples *src,
    const struct virtio_gpu_present_samples *dst)
{
    int matches = 0;

    if (src == NULL || dst == NULL)
        return 0;
    for (int i = 0; i < VIRTIO_GPU_PRESENT_SAMPLE_COUNT; i++) {
        uint32 rgb = src->p[i] & 0x00ffffffu;

        if (rgb != 0 && rgb == (dst->p[i] & 0x00ffffffu))
            matches++;
    }
    return matches;
}

static int virtio_gpu_clear_texture_rect(struct virtio_gpu *g,
                                         uint32 ctx_id,
                                         uint32 resource_id,
                                         uint32 x, uint32 y,
                                         uint32 w, uint32 h,
                                         uint32 color)
{
    uint32 cmds[VIRGL_CLEAR_TEXTURE_SIZE + 1];
    uint64 fence_id;

    if (resource_id == 0 || w == 0 || h == 0)
        return -EINVAL;

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_CLEAR_TEXTURE, 0,
                         VIRGL_CLEAR_TEXTURE_SIZE);
    cmds[1] = resource_id;
    cmds[2] = 0;
    cmds[3] = x;
    cmds[4] = y;
    cmds[5] = 0;
    cmds[6] = w;
    cmds[7] = h;
    cmds[8] = 1;
    cmds[9] = color;
    cmds[10] = 0;
    cmds[11] = 0;
    cmds[12] = 0;

    return virtio_gpu_submit_3d(g, ctx_id, cmds,
                                sizeof(cmds) / sizeof(cmds[0]),
                                fence_id) == 0 ? 0 : -EIO;
}

static int virtio_gpu_submit_blit_rect(struct virtio_gpu *g,
                                       uint32 ctx_id,
                                       uint32 dst_resource_id,
                                       uint32 dst_format,
                                       uint32 dst_x, uint32 dst_y,
                                       uint32 w, uint32 h,
                                       uint32 src_resource_id,
                                       uint32 src_format,
                                       uint32 src_x, uint32 src_y,
                                       uint64 fence_id)
{
    uint32 cmds[VIRGL_CMD_BLIT_SIZE + 1];

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE);
    cmds[1] = VIRGL_CMD_BLIT_S0_MASK(VIRGL_PIPE_MASK_RGBA) |
              VIRGL_CMD_BLIT_S0_FILTER(VIRGL_PIPE_TEX_FILTER_NEAREST);
    cmds[2] = 0;
    cmds[3] = 0;
    cmds[4] = dst_resource_id;
    cmds[5] = 0;
    cmds[6] = dst_format ? dst_format : VIRGL_FORMAT_B8G8R8A8_UNORM;
    cmds[7] = dst_x;
    cmds[8] = dst_y;
    cmds[9] = 0;
    cmds[10] = w;
    cmds[11] = h;
    cmds[12] = 1;
    cmds[13] = src_resource_id;
    cmds[14] = 0;
    cmds[15] = src_format ? src_format : VIRGL_FORMAT_B8G8R8A8_UNORM;
    cmds[16] = src_x;
    cmds[17] = src_y;
    cmds[18] = 0;
    cmds[19] = w;
    cmds[20] = h;
    cmds[21] = 1;
    return virtio_gpu_submit_3d(g, ctx_id, cmds,
                                sizeof(cmds) / sizeof(cmds[0]),
                                fence_id) == 0 ? 0 : -EIO;
}

static int virtio_gpu_submit_copy_rect(struct virtio_gpu *g,
                                       uint32 ctx_id,
                                       uint32 dst_resource_id,
                                       uint32 dst_x, uint32 dst_y,
                                       uint32 w, uint32 h,
                                       uint32 src_resource_id,
                                       uint32 src_x, uint32 src_y,
                                       uint64 fence_id)
{
    uint32 cmds[VIRGL_CMD_RESOURCE_COPY_REGION_SIZE + 1];

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0,
                         VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
    cmds[1] = dst_resource_id;
    cmds[2] = 0;
    cmds[3] = dst_x;
    cmds[4] = dst_y;
    cmds[5] = 0;
    cmds[6] = src_resource_id;
    cmds[7] = 0;
    cmds[8] = src_x;
    cmds[9] = src_y;
    cmds[10] = 0;
    cmds[11] = w;
    cmds[12] = h;
    cmds[13] = 1;
    return virtio_gpu_submit_3d(g, ctx_id, cmds,
                                sizeof(cmds) / sizeof(cmds[0]),
                                fence_id) == 0 ? 0 : -EIO;
}

static int virtio_gpu_submit_copy_transfer_rect(struct virtio_gpu *g,
                                                uint32 ctx_id,
                                                struct virtio_gpu_resource *dst,
                                                uint32 dst_x, uint32 dst_y,
                                                uint32 w, uint32 h,
                                                struct virtio_gpu_resource *src,
                                                uint32 src_x, uint32 src_y,
                                                uint64 fence_id)
{
    uint32 cmds[VIRGL_COPY_TRANSFER3D_SIZE + 1];
    uint64 src_offset;

    if (dst == NULL || src == NULL || dst->width == 0 || src->width == 0)
        return -EINVAL;
    src_offset = ((uint64)src_y * src->width + src_x) * sizeof(uint32);
    if (src_offset > 0xffffffffu)
        return -EINVAL;

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_COPY_TRANSFER3D, 0,
                         VIRGL_COPY_TRANSFER3D_SIZE);
    cmds[1] = dst->id;
    cmds[2] = 0;
    cmds[3] = 0;
    cmds[4] = dst->width * sizeof(uint32);
    cmds[5] = dst->width * dst->height * sizeof(uint32);
    cmds[6] = dst_x;
    cmds[7] = dst_y;
    cmds[8] = 0;
    cmds[9] = w;
    cmds[10] = h;
    cmds[11] = 1;
    cmds[12] = src->id;
    cmds[13] = (uint32)src_offset;
    cmds[14] = VIRGL_COPY_TRANSFER3D_FLAGS_SYNCHRONIZED |
               VIRGL_COPY_TRANSFER3D_FLAGS_READ_FROM_HOST;
    return virtio_gpu_submit_3d(g, ctx_id, cmds,
                                sizeof(cmds) / sizeof(cmds[0]),
                                fence_id) == 0 ? 0 : -EIO;
}

static int virtio_gpu_submit_present_copy_method(
    struct virtio_gpu *g, uint32 ctx_id,
    struct virtio_gpu_resource *dst, uint32 dst_x, uint32 dst_y,
    uint32 w, uint32 h,
    struct virtio_gpu_resource *src, uint32 src_x, uint32 src_y,
    uint64 fence_id, int use_copy_transfer, int use_blit)
{
    if (dst == NULL || src == NULL)
        return -EINVAL;
    if (use_copy_transfer) {
        return virtio_gpu_submit_copy_transfer_rect(g, ctx_id, dst, dst_x,
                                                    dst_y, w, h, src, src_x,
                                                    src_y, fence_id);
    }
    if (use_blit) {
        return virtio_gpu_submit_blit_rect(
            g, ctx_id, dst->id, dst->format, dst_x, dst_y, w, h,
            src->id, src->format, src_x, src_y, fence_id);
    }
    return virtio_gpu_submit_copy_rect(g, ctx_id, dst->id, dst_x, dst_y,
                                       w, h, src->id, src_x, src_y,
                                       fence_id);
}

static int virtio_gpu_try_pageflip_copy(
    struct virtio_gpu *g, struct virtio_gpu_resource *src,
    struct virtio_gpu_resource *scanout, uint32 ctx_id,
    uint32 src_x, uint32 src_y, uint32 dst_x, uint32 dst_y,
    uint32 w, uint32 h, int use_copy_transfer, int use_blit)
{
    struct virtio_gpu_resource *base;
    struct virtio_gpu_resource *flip = NULL;
    uint32 base_resource_id;
    uint32 flip_slot = 0;
    uint64 fence_id;
    int ret;
    int base_attached = 0;
    int flip_attached = 0;
    int need_base_copy = 1;
    uint32 base_slot = 0;
    int base_is_valid_flip = 0;
    int same_rect_as_flip = 0;
    int flip_has_current_base = 0;
    uint32 base_generation = g->present_base_generation;
    uint32 flush_x = 0;
    uint32 flush_y = 0;
    uint32 flush_w = 0;
    uint32 flush_h = 0;
    int validate_copy = virtio_gpu_pageflip_validate_enabled();
    static int validation_state;
    static int fail_logs;
    static int success_logs;

    if (!virtio_gpu_pageflip_copy_enabled() ||
        src == NULL || scanout == NULL || !scanout->is_3d ||
        scanout->width == 0 || scanout->height == 0)
        return -EOPNOTSUPP;
    if (dst_x > scanout->width || w > scanout->width - dst_x ||
        dst_y > scanout->height || h > scanout->height - dst_y)
        return -EINVAL;
    flush_w = scanout->width;
    flush_h = scanout->height;
    if (validate_copy && validation_state < 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_force_copy"))
        return -EOPNOTSUPP;

    ret = virtio_gpu_ensure_present_flip_resource(
        g, scanout->width, scanout->height, &flip, &flip_slot);
    if (ret != 0) {
        if (fail_logs < 8) {
            printf("virtio_gpu: pageflip-copy setup failed ret=%d screen=%ux%u\n",
                   ret, scanout->width, scanout->height);
            fail_logs++;
        }
        return ret;
    }

    if (virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_base_scanout")) {
        base = scanout;
        base_resource_id = base->id;
    } else {
        spin_lock(&g->lock);
        base = virtio_gpu_lookup_resource_locked(g,
                                                 g->bound_scanout_resource_id);
        if (base == NULL || !base->attached ||
            base->width != scanout->width || base->height != scanout->height)
            base = scanout;
        base_resource_id = base->id;
        spin_unlock(&g->lock);
    }

    base_is_valid_flip =
        virtio_gpu_present_flip_slot_for_id(g, base_resource_id, &base_slot) &&
        g->present_flip_valid[base_slot];
    same_rect_as_flip =
        g->present_flip_valid[flip_slot] &&
        g->present_flip_rect_x[flip_slot] == dst_x &&
        g->present_flip_rect_y[flip_slot] == dst_y &&
        g->present_flip_rect_w[flip_slot] == w &&
        g->present_flip_rect_h[flip_slot] == h;
    flip_has_current_base =
        g->present_flip_valid[flip_slot] &&
        g->present_flip_base_generation[flip_slot] == base_generation;
    if (base_is_valid_flip && same_rect_as_flip &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_full_copy"))
        need_base_copy = 0;
    if (same_rect_as_flip && flip_has_current_base &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_pageflip_full_copy"))
        need_base_copy = 0;

    if (need_base_copy &&
        base_resource_id != scanout->id && base_resource_id != src->id) {
        ret = virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id, base_resource_id);
        if (ret != 0) {
            if (fail_logs < 8) {
                printf("virtio_gpu: pageflip-copy attach-base failed ret=%d ctx=%u base=%u\n",
                       ret, ctx_id, base_resource_id);
                fail_logs++;
            }
            return -EIO;
        }
        base_attached = 1;
    }
    ret = virtio_gpu_context_resource(
        g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id, flip->id);
    if (ret != 0) {
        if (fail_logs < 8) {
            printf("virtio_gpu: pageflip-copy attach-flip failed ret=%d ctx=%u flip=%u\n",
                   ret, ctx_id, flip->id);
            fail_logs++;
        }
        ret = -EIO;
        goto out;
    }
    flip_attached = 1;

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (need_base_copy) {
        ret = virtio_gpu_submit_present_copy_method(
            g, ctx_id, flip, 0, 0, scanout->width, scanout->height,
            base, 0, 0, fence_id, 0, use_blit);
        if (ret != 0) {
            if (fail_logs < 8) {
                printf("virtio_gpu: pageflip-copy base-copy failed ret=%d ctx=%u base=%u flip=%u size=%ux%u mode=%s\n",
                       ret, ctx_id, base_resource_id, flip->id,
                       scanout->width, scanout->height,
                       use_blit ? "blit" : "copy");
                fail_logs++;
            }
            virtio_gpu_invalidate_present_flip_slot(g, flip_slot);
            goto out;
        }
        g->present_flip_base_generation[flip_slot] = base_generation;
    } else {
        flush_x = dst_x;
        flush_y = dst_y;
        flush_w = w;
        flush_h = h;
    }

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    ret = virtio_gpu_submit_present_copy_method(
        g, ctx_id, flip, dst_x, dst_y, w, h, src, src_x, src_y,
        fence_id, use_copy_transfer, use_blit);
    if (ret != 0) {
        if (fail_logs < 8) {
            printf("virtio_gpu: pageflip-copy overlay-copy failed ret=%d ctx=%u src=%u flip=%u rect=%u,%u %ux%u mode=%s\n",
                   ret, ctx_id, src->id, flip->id, dst_x, dst_y, w, h,
                   use_copy_transfer ? "copy-transfer" :
                                       (use_blit ? "blit" : "copy"));
            fail_logs++;
        }
        virtio_gpu_invalidate_present_flip_slot(g, flip_slot);
        goto out;
    }

    ret = virtio_gpu_set_scanout(g, 0, flip, 0, 0, scanout->width,
                                 scanout->height) == 0 ? 0 : -EIO;
    if (ret != 0) {
        if (fail_logs < 8) {
            printf("virtio_gpu: pageflip-copy set-scanout failed ret=%d flip=%u size=%ux%u\n",
                   ret, flip->id, scanout->width, scanout->height);
            fail_logs++;
        }
        goto out;
    }
    g->bound_scanout_resource_id = flip->id;
    g->present_flip_index = flip_slot;
    g->present_flip_valid[flip_slot] = 1;
    g->present_flip_rect_x[flip_slot] = dst_x;
    g->present_flip_rect_y[flip_slot] = dst_y;
    g->present_flip_rect_w[flip_slot] = w;
    g->present_flip_rect_h[flip_slot] = h;
    ret = virtio_gpu_resource_flush(g, flip, flush_x, flush_y, flush_w,
                                    flush_h) == 0 ? 0 : -EIO;
    if (ret != 0 && fail_logs < 8) {
        printf("virtio_gpu: pageflip-copy flush failed ret=%d flip=%u rect=%u,%u %ux%u\n",
               ret, flip->id, flush_x, flush_y, flush_w, flush_h);
        fail_logs++;
    }
    if (ret == 0 && validate_copy && validation_state == 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_force_copy")) {
        struct virtio_gpu_present_samples src_samples;
        struct virtio_gpu_present_samples flip_samples;
        int src_ret = virtio_gpu_resource_host_samples(
            g, src, ctx_id, src_x, src_y, w, h, &src_samples);
        int flip_ret = virtio_gpu_resource_host_samples(
            g, flip, ctx_id, dst_x, dst_y, w, h, &flip_samples);
        int matches = (src_ret == 0 && flip_ret == 0) ?
            virtio_gpu_present_sample_matches(&src_samples,
                                              &flip_samples) : 0;
        int required_matches =
            (src_ret == 0 && src_samples.unique_nonblack >= 2 &&
             src_samples.nonblack >= 3) ? 2 : 1;

        if (src_ret != 0 || flip_ret != 0 ||
            (src_samples.nonblack > 0 && matches < required_matches)) {
            validation_state = -1;
            if (fail_logs < 4) {
                printf("virtio_gpu: pageflip-copy validation failed after-scanout src_ret=%d flip_ret=%d src_nonblack=%u flip_nonblack=%u matches=%d required=%d src=%08x,%08x,%08x,%08x,%08x flip=%08x,%08x,%08x,%08x,%08x; disabling pageflip-copy for this boot\n",
                       src_ret, flip_ret,
                       src_ret == 0 ? src_samples.nonblack : 0,
                       flip_ret == 0 ? flip_samples.nonblack : 0,
                       matches, required_matches,
                       src_ret == 0 ? src_samples.p[0] : 0,
                       src_ret == 0 ? src_samples.p[1] : 0,
                       src_ret == 0 ? src_samples.p[2] : 0,
                       src_ret == 0 ? src_samples.p[3] : 0,
                       src_ret == 0 ? src_samples.p[4] : 0,
                       flip_ret == 0 ? flip_samples.p[0] : 0,
                       flip_ret == 0 ? flip_samples.p[1] : 0,
                       flip_ret == 0 ? flip_samples.p[2] : 0,
                       flip_ret == 0 ? flip_samples.p[3] : 0,
                       flip_ret == 0 ? flip_samples.p[4] : 0);
                fail_logs++;
            }
            (void)virtio_gpu_set_scanout(g, 0, scanout, 0, 0,
                                         scanout->width, scanout->height);
            g->bound_scanout_resource_id = scanout->id;
            virtio_gpu_invalidate_present_flip_slot(g, flip_slot);
            ret = -EOPNOTSUPP;
            goto out;
        }
        if (matches >= required_matches)
            validation_state = 1;
    }
    if (ret == 0 && success_logs < 8) {
        printf("virtio_gpu: pageflip-copy present src=%u base=%u flip=%u slot=%u rect=%u,%u %ux%u screen=%ux%u base_copy=%d base_gen=%u flush=%u,%u %ux%u\n",
               src->id, base_resource_id, flip->id, flip_slot,
               dst_x, dst_y, w, h, scanout->width, scanout->height,
               need_base_copy, base_generation, flush_x, flush_y, flush_w,
               flush_h);
        success_logs++;
    }
    if (ret != 0)
        virtio_gpu_invalidate_present_flip_slot(g, flip_slot);

out:
    if (flip_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, flip->id);
    if (base_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, base_resource_id);
    return ret;
}

static void virtio_gpu_probe_present_source(struct virtio_gpu *g,
                                            struct virtio_gpu_resource *src,
                                            uint32 ctx_id)
{
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 p0, p1, p2, p3, p4;
    uint32 nonblack = 0;
    int ret;
    static int probe_logs;

    if (!virtio_gpu_cmdline_enabled("virtio_gpu_present_probe") ||
        probe_logs >= 12 || src == NULL || src->width == 0 ||
        src->height == 0 || src->pages == NULL || src->npages == 0)
        return;

    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer->hdr.ctx_id = ctx_id;
    transfer->box.x = 0;
    transfer->box.y = 0;
    transfer->box.z = 0;
    transfer->box.w = src->width;
    transfer->box.h = src->height;
    transfer->box.d = 1;
    transfer->offset = 0;
    transfer->resource_id = src->id;
    transfer->level = 0;
    transfer->stride = src->width * sizeof(uint32);
    transfer->layer_stride = (uint64)transfer->stride * src->height;
    ret = virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                            resp, sizeof(*resp),
                            VIRTIO_GPU_RESP_OK_NODATA);
    p0 = virtio_gpu_resource_sample_pixel(src, src->width / 2,
                                          src->height / 2);
    p1 = virtio_gpu_resource_sample_pixel(src, src->width / 4,
                                          src->height / 4);
    p2 = virtio_gpu_resource_sample_pixel(src, (src->width * 3) / 4,
                                          src->height / 4);
    p3 = virtio_gpu_resource_sample_pixel(src, src->width / 4,
                                          (src->height * 3) / 4);
    p4 = virtio_gpu_resource_sample_pixel(src, (src->width * 3) / 4,
                                          (src->height * 3) / 4);
    if ((p0 & 0x00ffffffu) != 0) nonblack++;
    if ((p1 & 0x00ffffffu) != 0) nonblack++;
    if ((p2 & 0x00ffffffu) != 0) nonblack++;
    if ((p3 & 0x00ffffffu) != 0) nonblack++;
    if ((p4 & 0x00ffffffu) != 0) nonblack++;
    printf("virtio_gpu: present source probe resource=%u ctx=%u ret=%d nonblack=%u samples=%08x,%08x,%08x,%08x,%08x size=%ux%u\n",
           src->id, ctx_id, ret == 0 ? 0 : -EIO, nonblack,
           p0, p1, p2, p3, p4, src->width, src->height);
    probe_logs++;
}

static void virtio_gpu_probe_present_dst(struct virtio_gpu *g,
                                         struct virtio_gpu_resource *dst,
                                         uint32 ctx_id,
                                         uint32 x, uint32 y,
                                         uint32 w, uint32 h)
{
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)g->resp_page;
    uint32 p0, p1, p2, p3, p4;
    uint32 nonblack = 0;
    int ret;
    static int probe_logs;

    if (!virtio_gpu_cmdline_enabled("virtio_gpu_present_probe") ||
        probe_logs >= 12 || dst == NULL || dst->width == 0 ||
        dst->height == 0 ||
        (dst->backing == NULL && (dst->pages == NULL || dst->npages == 0)))
        return;

    memset(transfer, 0, sizeof(*transfer));
    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer->hdr.ctx_id = ctx_id;
    transfer->box.x = x;
    transfer->box.y = y;
    transfer->box.z = 0;
    transfer->box.w = w;
    transfer->box.h = h;
    transfer->box.d = 1;
    transfer->offset = ((uint64)y * dst->width + x) * sizeof(uint32);
    transfer->resource_id = dst->id;
    transfer->level = 0;
    transfer->stride = dst->width * sizeof(uint32);
    transfer->layer_stride = (uint64)transfer->stride * dst->height;
    ret = virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0, false,
                            resp, sizeof(*resp),
                            VIRTIO_GPU_RESP_OK_NODATA);
    p0 = virtio_gpu_resource_sample_pixel(dst, x + w / 2, y + h / 2);
    p1 = virtio_gpu_resource_sample_pixel(dst, x + w / 4, y + h / 4);
    p2 = virtio_gpu_resource_sample_pixel(dst, x + (w * 3) / 4, y + h / 4);
    p3 = virtio_gpu_resource_sample_pixel(dst, x + w / 4, y + (h * 3) / 4);
    p4 = virtio_gpu_resource_sample_pixel(dst, x + (w * 3) / 4,
                                          y + (h * 3) / 4);
    if ((p0 & 0x00ffffffu) != 0) nonblack++;
    if ((p1 & 0x00ffffffu) != 0) nonblack++;
    if ((p2 & 0x00ffffffu) != 0) nonblack++;
    if ((p3 & 0x00ffffffu) != 0) nonblack++;
    if ((p4 & 0x00ffffffu) != 0) nonblack++;
    printf("virtio_gpu: present dst probe resource=%u ctx=%u ret=%d nonblack=%u samples=%08x,%08x,%08x,%08x,%08x rect=%u,%u %ux%u\n",
           dst->id, ctx_id, ret == 0 ? 0 : -EIO, nonblack,
           p0, p1, p2, p3, p4, x, y, w, h);
    probe_logs++;
}

int virtio_gpu_copy_resource_to_scanout(uint32 src_resource_id,
                                        uint32 src_x, uint32 src_y,
                                        uint32 dst_x, uint32 dst_y,
                                        uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *src;
    struct virtio_gpu_resource *dst;
    struct virtio_gpu_context *ctx;
    uint32 ctx_id = 0;
    uint32 dst_resource_id;
    uint32 src_width = 0;
    uint32 src_height = 0;
    uint32 dst_width = 0;
    uint32 dst_height = 0;
    uint64 fence_id;
    uint64 src_submit_fence = 0;
    uint64 completed_fence = 0;
    int should_wait_src_fence = 0;
    int src_present = 0;
    int src_is_attached = 0;
    int dst_present = 0;
    int dst_is_attached = 0;
    int src_attached = 0;
    int used_present_ctx = 0;
    int ret = -EIO;
    static int invalid_logs;
    static int debug_logs;
    static int copy_validation_state;
    static int copy_validation_logs;
    int validate_copy =
        virtio_gpu_cmdline_enabled("virtio_gpu_present_validate_copy");
    int use_copy_transfer =
        virtio_gpu_cmdline_enabled("virtio_gpu_present_copy_transfer");
    int use_blit =
        virtio_gpu_cmdline_enabled("virtio_gpu_present_blit") &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_copy_region");
    int unbind_scanout_copy =
        virtio_gpu_cmdline_enabled("virtio_gpu_unbind_scanout_copy");
    int rearm_scanout_copy =
        virtio_gpu_cmdline_enabled("virtio_gpu_rearm_scanout_copy") &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_no_rearm_scanout_copy");

    if (src_resource_id == 0 || w == 0 || h == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PRESENT_COPY);
    spin_lock(&g->lock);
    src = virtio_gpu_lookup_resource_locked(g, src_resource_id);
    dst = g->scanout_resource;
    src_present = src != NULL;
    dst_present = dst != NULL;
    if (src != NULL) {
        src_width = src->width;
        src_height = src->height;
        src_is_attached = src->attached;
        src_submit_fence = src->last_submit_fence;
    }
    if (dst != NULL) {
        dst_width = dst->width;
        dst_height = dst->height;
        dst_is_attached = dst->attached;
    }
    if (src == NULL || dst == NULL || !src->attached || !dst->attached ||
        src_x > src->width || w > src->width - src_x ||
        src_y > src->height || h > src->height - src_y ||
        dst_x > dst->width || w > dst->width - dst_x ||
        dst_y > dst->height || h > dst->height - dst_y) {
        spin_unlock(&g->lock);
        if (invalid_logs < 4) {
            printf("virtio_gpu: resource-copy reject src=%u present=%d attached=%d size=%ux%u src_rect=%u,%u %ux%u dst_present=%d dst_attached=%d dst_size=%ux%u dst_rect=%u,%u\n",
                   src_resource_id, src_present, src_is_attached,
                   src_width, src_height, src_x, src_y, w, h,
                   dst_present, dst_is_attached, dst_width, dst_height,
                   dst_x, dst_y);
            invalid_logs++;
        }
        ret = -EINVAL;
        goto out;
    }
    dst_resource_id = dst->id;
    if (src->ctx_id != 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_separate_ctx")) {
        ctx = virtio_gpu_lookup_context_locked(g, src->ctx_id);
        if (ctx != NULL && !ctx->failed)
            ctx_id = src->ctx_id;
    }
    if (debug_logs < 8) {
        printf("virtio_gpu: present copy src=%u ctx=%u kind=%s fmt=%u bind=0x%x target=%u %ux%u -> dst=%u kind=%s fmt=%u bind=0x%x target=%u %ux%u rect src=%u,%u dst=%u,%u %ux%u mode=%s\n",
               src_resource_id, ctx_id, src->is_3d ? "3d" : "2d",
               src->format, src->bind, src->target, src->width, src->height,
               dst->id, dst->is_3d ? "3d" : "2d", dst->format, dst->bind,
               dst->target, dst->width, dst->height, src_x, src_y, dst_x,
               dst_y, w, h,
               use_copy_transfer ? "copy-transfer" :
                                    (use_blit ? "blit" : "copy"));
        debug_logs++;
    }
    completed_fence = g->stats.last_fence;
    should_wait_src_fence = src_submit_fence != 0 &&
        src_submit_fence > completed_fence &&
        virtio_gpu_async_pending(g) &&
        virtio_gpu_async_newest_fence(g) <= src_submit_fence;
    spin_unlock(&g->lock);

    if (virtio_gpu_cmdline_enabled("virtio_gpu_present_readback_copy")) {
        ret = -EINVAL;
        goto out;
    }

    /*
     * Mesa submits virgl command streams asynchronously.  Linux/Alpine gets an
     * implicit dma-buf fence before scanout import; xv6's native Wayland path
     * currently hands the compositor only the resource fd.  Drain the one
     * pending virgl submit before copying that resource into the scanout, so we
     * present the rendered frame instead of the previous/empty contents.
     *
     * By default this drains the WHOLE async ring with wait=1, which also waits
     * for the previous frame's async RESOURCE_FLUSH to retire on the host (its
     * ~vsync).  That re-serialises the pipeline at every frame boundary and is
     * the dominant reason async flush/transfer buy nothing: compose(N) blocks on
     * flush(N-1).  should_wait_src_fence already precisely captures "the source
     * buffer's render is still in flight in the ring" (src_submit_fence newer
     * than the last completed fence and still pending).  In minimal-drain mode
     * we drain only when that is true, so a compose whose source is already
     * complete proceeds immediately and overlaps the prior flush's host vsync.
     * Opt-in via "virtio_gpu_present_minimal_drain"; the blanket drain remains
     * the default and "virtio_gpu_present_no_drain" still force-skips it.
     */
    {
        int minimal_drain =
            virtio_gpu_cmdline_enabled("virtio_gpu_present_minimal_drain");
        int want_drain = should_wait_src_fence ||
            (!minimal_drain &&
             !virtio_gpu_cmdline_enabled("virtio_gpu_present_no_drain"));

        if (want_drain) {
            ret = virtio_gpu_drain_async_submit(g, 1);
            if (ret != 0) {
                ret = -EIO;
                goto out;
            }
        }
    }

    if (ctx_id == 0) {
        ret = virtio_gpu_ensure_present_context(g, &ctx_id);
        if (ret != 0)
            goto out;
        used_present_ctx = 1;
        if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                        ctx_id, src_resource_id) != 0) {
            ret = -EIO;
            goto out_drop_context;
        }
        src_attached = 1;
    }

    ret = virtio_gpu_ensure_scanout_attached(g, ctx_id, dst_resource_id);
    if (ret != 0)
        goto out_detach;

    virtio_gpu_probe_present_source(g, src, ctx_id);

    if (virtio_gpu_cmdline_enabled("virtio_gpu_present_clear_probe")) {
        uint32 pw = w < 64 ? w : 64;
        uint32 ph = h < 64 ? h : 64;
        int clear_ret = virtio_gpu_clear_texture_rect(
            g, ctx_id, dst_resource_id, dst_x, dst_y, pw, ph, 0xffffffffu);
        int dst_nonblack = clear_ret == 0 ?
            virtio_gpu_resource_host_nonblack(g, dst, ctx_id,
                                              dst_x, dst_y, pw, ph) :
            clear_ret;

        printf("virtio_gpu: present clear-probe resource=%u ctx=%u ret=%d nonblack=%d rect=%u,%u %ux%u\n",
               dst_resource_id, ctx_id, clear_ret, dst_nonblack,
               dst_x, dst_y, pw, ph);
        ret = -EOPNOTSUPP;
        goto out_detach;
    }

    if (virtio_gpu_cmdline_enabled("virtio_gpu_present_temp_blit_probe")) {
        struct virtio_gpu_resource *tmp = NULL;
        struct virtio_gpu_present_samples src_samples;
        struct virtio_gpu_present_samples tmp_samples;
        uint64 tmp_fence;
        int tmp_ret;
        int tmp_attached = 0;
        int tmp_ctx_attached = 0;
        int src_ret;
        int sample_ret;
        int matches = 0;

        tmp_ret = virtio_gpu_resource_create_3d_backing(
            g, w, h, src->format ? src->format : VIRGL_FORMAT_B8G8R8A8_UNORM,
            VIRTIO_GPU_PIPE_BIND_RENDER_TARGET |
            VIRTIO_GPU_PIPE_BIND_SAMPLER_VIEW |
            VIRTIO_GPU_PIPE_BIND_SHARED |
            VIRTIO_GPU_PIPE_BIND_LINEAR, &tmp);
        if (tmp_ret == 0) {
            tmp_ret = virtio_gpu_resource_attach_backing(g, tmp);
            tmp_attached = tmp_ret == 0;
        }
        if (tmp_ret == 0) {
            memset(tmp->backing, 0, tmp->backing_len);
            tmp_ret = virtio_gpu_resource_transfer_3d(g, tmp, 0, 0, w, h);
        }
        if (tmp_ret == 0) {
            tmp_ret = virtio_gpu_context_resource(
                g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id, tmp->id);
            tmp_ctx_attached = tmp_ret == 0;
        }
        if (tmp_ret == 0) {
            spin_lock(&g->lock);
            tmp_fence = ++g->next_fence_id;
            spin_unlock(&g->lock);
            if (use_copy_transfer) {
                tmp_ret = virtio_gpu_submit_copy_transfer_rect(
                    g, ctx_id, tmp, 0, 0, w, h, src, src_x, src_y,
                    tmp_fence);
            } else if (use_blit) {
                tmp_ret = virtio_gpu_submit_blit_rect(
                    g, ctx_id, tmp->id, tmp->format, 0, 0, w, h,
                    src_resource_id, src->format, src_x, src_y, tmp_fence);
            } else {
                tmp_ret = virtio_gpu_submit_copy_rect(
                    g, ctx_id, tmp->id, 0, 0, w, h,
                    src_resource_id, src_x, src_y, tmp_fence);
            }
        }
        src_ret = virtio_gpu_resource_host_samples(g, src, ctx_id,
                                                   src_x, src_y, w, h,
                                                   &src_samples);
        sample_ret = tmp_ret == 0 ?
            virtio_gpu_resource_host_samples(g, tmp, ctx_id, 0, 0, w, h,
                                             &tmp_samples) : tmp_ret;
        if (src_ret == 0 && sample_ret == 0)
            matches = virtio_gpu_present_sample_matches(&src_samples,
                                                        &tmp_samples);
        printf("virtio_gpu: present temp-%s-probe create=%d attached=%d ctx_attached=%d sample=%d matches=%d src_nonblack=%u tmp_nonblack=%u src=%08x,%08x,%08x,%08x,%08x tmp=%08x,%08x,%08x,%08x,%08x\n",
               use_copy_transfer ? "copy-transfer" :
                                   (use_blit ? "blit" : "copy"),
               tmp_ret, tmp_attached,
               tmp_ctx_attached, sample_ret, matches,
               src_ret == 0 ? src_samples.nonblack : 0,
               sample_ret == 0 ? tmp_samples.nonblack : 0,
               src_ret == 0 ? src_samples.p[0] : 0,
               src_ret == 0 ? src_samples.p[1] : 0,
               src_ret == 0 ? src_samples.p[2] : 0,
               src_ret == 0 ? src_samples.p[3] : 0,
               src_ret == 0 ? src_samples.p[4] : 0,
               sample_ret == 0 ? tmp_samples.p[0] : 0,
               sample_ret == 0 ? tmp_samples.p[1] : 0,
               sample_ret == 0 ? tmp_samples.p[2] : 0,
               sample_ret == 0 ? tmp_samples.p[3] : 0,
               sample_ret == 0 ? tmp_samples.p[4] : 0);
        if (tmp_ctx_attached)
            (void)virtio_gpu_context_resource(
                g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, tmp->id);
        if (tmp != NULL)
            (void)virtio_gpu_resource_unref(g, tmp);
        ret = -EOPNOTSUPP;
        goto out_detach;
    }

    ret = virtio_gpu_try_pageflip_copy(g, src, dst, ctx_id, src_x, src_y,
                                       dst_x, dst_y, w, h,
                                       use_copy_transfer, use_blit);
    if (ret == 0) {
        spin_lock(&g->lock);
        virtio_gpu_note_diagnostic_present_locked(
            g, ctx_id, src_resource_id, src_x, src_y, dst_x, dst_y, w, h);
        spin_unlock(&g->lock);
        goto out_detach;
    }

    if (g->bound_scanout_resource_id != dst_resource_id) {
        if (virtio_gpu_set_scanout(g, 0, dst, 0, 0, dst_width,
                                   dst_height) != 0) {
            ret = -EIO;
            goto out_detach;
        }
        g->bound_scanout_resource_id = dst_resource_id;
    }

    spin_lock(&g->lock);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (unbind_scanout_copy &&
        g->bound_scanout_resource_id == dst_resource_id) {
        if (virtio_gpu_set_scanout(g, 0, NULL, 0, 0, 0, 0) != 0) {
            ret = -EIO;
            goto out_detach;
        }
        g->bound_scanout_resource_id = 0;
    }
    if (use_copy_transfer) {
        ret = virtio_gpu_submit_copy_transfer_rect(g, ctx_id, dst, dst_x,
                                                   dst_y, w, h, src, src_x,
                                                   src_y, fence_id);
    } else if (use_blit) {
        ret = virtio_gpu_submit_blit_rect(g, ctx_id, dst_resource_id,
                                          dst->format, dst_x, dst_y, w, h,
                                          src_resource_id, src->format,
                                          src_x, src_y, fence_id);
    } else {
        ret = virtio_gpu_submit_copy_rect(g, ctx_id, dst_resource_id,
                                          dst_x, dst_y, w, h,
                                          src_resource_id, src_x, src_y,
                                          fence_id);
    }
    if (validate_copy && ret == 0 &&
        copy_validation_state < 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_force_copy")) {
        ret = -EOPNOTSUPP;
        goto out_detach;
    }
    if (validate_copy && ret == 0 && copy_validation_state == 0 &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_force_copy")) {
        struct virtio_gpu_present_samples src_samples;
        struct virtio_gpu_present_samples dst_samples;
        int src_ret = virtio_gpu_resource_host_samples(
            g, src, ctx_id, src_x, src_y, w, h, &src_samples);
        int dst_ret = virtio_gpu_resource_host_samples(
            g, dst, ctx_id, dst_x, dst_y, w, h, &dst_samples);
        int matches = (src_ret == 0 && dst_ret == 0) ?
            virtio_gpu_present_sample_matches(&src_samples, &dst_samples) : 0;
        int required_matches =
            (src_ret == 0 && src_samples.unique_nonblack >= 2 &&
             src_samples.nonblack >= 3) ? 2 : 1;

        if (src_ret != 0 || dst_ret != 0 ||
            (src_samples.nonblack > 0 && matches < required_matches)) {
            copy_validation_state = -1;
            if (copy_validation_logs < 4) {
                printf("virtio_gpu: virgl resource-copy validation failed src_ret=%d dst_ret=%d src_nonblack=%u dst_nonblack=%u matches=%d required=%d src=%08x,%08x,%08x,%08x,%08x dst=%08x,%08x,%08x,%08x,%08x; falling back to readback present\n",
                       src_ret, dst_ret,
                       src_ret == 0 ? src_samples.nonblack : 0,
                       dst_ret == 0 ? dst_samples.nonblack : 0,
                       matches, required_matches,
                       src_ret == 0 ? src_samples.p[0] : 0,
                       src_ret == 0 ? src_samples.p[1] : 0,
                       src_ret == 0 ? src_samples.p[2] : 0,
                       src_ret == 0 ? src_samples.p[3] : 0,
                       src_ret == 0 ? src_samples.p[4] : 0,
                       dst_ret == 0 ? dst_samples.p[0] : 0,
                       dst_ret == 0 ? dst_samples.p[1] : 0,
                       dst_ret == 0 ? dst_samples.p[2] : 0,
                       dst_ret == 0 ? dst_samples.p[3] : 0,
                       dst_ret == 0 ? dst_samples.p[4] : 0);
                copy_validation_logs++;
            }
            ret = -EOPNOTSUPP;
            goto out_detach;
        }
        if (matches >= required_matches)
            copy_validation_state = 1;
    }
    if (ret == 0 &&
        (unbind_scanout_copy ||
         rearm_scanout_copy)) {
        /*
         * Some QEMU/virgl hosts need a no-mode-change SET_SCANOUT refresh hint
         * after a 3D copy into the already-bound scanout.  It is expensive on
         * WSL/NVIDIA and the resource flush is sufficient there, so keep it
         * available as virtio_gpu_rearm_scanout_copy=1 rather than defaulting
         * to it every frame.
         */
        ret = virtio_gpu_set_scanout(g, 0, dst, 0, 0, dst_width,
                                     dst_height) == 0 ? 0 : -EIO;
        if (ret == 0)
            g->bound_scanout_resource_id = dst_resource_id;
    }
    if (ret == 0)
        virtio_gpu_probe_present_dst(g, dst, ctx_id, dst_x, dst_y, w, h);
    if (ret == 0)
        ret = virtio_gpu_resource_flush(g, dst, dst_x, dst_y, w, h) == 0 ?
            0 : -EIO;
    if (ret == 0) {
        spin_lock(&g->lock);
        virtio_gpu_note_diagnostic_present_locked(
            g, ctx_id, src_resource_id, src_x, src_y, dst_x, dst_y, w, h);
        spin_unlock(&g->lock);
    }

out_detach:
    if (src_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, src_resource_id);
out_drop_context:
    if (ret != 0 && used_present_ctx)
        virtio_gpu_drop_present_context(g, ctx_id);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

int virtio_gpu_copy_resource_to_resource(uint32 src_resource_id,
                                         uint32 dst_resource_id,
                                         uint32 src_x, uint32 src_y,
                                         uint32 dst_x, uint32 dst_y,
                                         uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *src;
    struct virtio_gpu_resource *dst;
    struct virtio_gpu_context *ctx;
    uint32 ctx_id = 0;
    uint64 src_submit_fence = 0;
    uint64 completed_fence = 0;
    uint64 fence_id;
    int should_wait_src_fence = 0;
    int src_attached = 0;
    int dst_attached = 0;
    int used_present_ctx = 0;
    int ret = -EIO;
    static int copy_logs;
    int use_copy_transfer =
        virtio_gpu_cmdline_enabled("virtio_gpu_present_copy_transfer");
    int use_blit = virtio_gpu_cmdline_enabled("virtio_gpu_present_blit") &&
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_copy_region");

    if (src_resource_id == 0 || dst_resource_id == 0 ||
        src_resource_id == dst_resource_id || w == 0 || h == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PRESENT_COPY);
    spin_lock(&g->lock);
    src = virtio_gpu_lookup_resource_locked(g, src_resource_id);
    dst = virtio_gpu_lookup_resource_locked(g, dst_resource_id);
    if (src == NULL || dst == NULL || !src->attached || !dst->attached ||
        src_x > src->width || w > src->width - src_x ||
        src_y > src->height || h > src->height - src_y ||
        dst_x > dst->width || w > dst->width - dst_x ||
        dst_y > dst->height || h > dst->height - dst_y) {
        spin_unlock(&g->lock);
        ret = -EINVAL;
        goto out;
    }
    if (src->ctx_id != 0) {
        ctx = virtio_gpu_lookup_context_locked(g, src->ctx_id);
        if (ctx != NULL && !ctx->failed)
            ctx_id = src->ctx_id;
    }
    src_submit_fence = src->last_submit_fence;
    completed_fence = g->stats.last_fence;
    should_wait_src_fence = src_submit_fence != 0 &&
        src_submit_fence > completed_fence &&
        virtio_gpu_async_pending(g) &&
        virtio_gpu_async_newest_fence(g) <= src_submit_fence;
    if (copy_logs < 8) {
        printf("virtio_gpu: resource-copy-to-resource src=%u dst=%u ctx=%u rect src=%u,%u dst=%u,%u %ux%u\n",
               src_resource_id, dst_resource_id, ctx_id, src_x, src_y,
               dst_x, dst_y, w, h);
        copy_logs++;
    }
    spin_unlock(&g->lock);

    if (should_wait_src_fence ||
        !virtio_gpu_cmdline_enabled("virtio_gpu_present_no_drain")) {
        ret = virtio_gpu_drain_async_submit(g, 1);
        if (ret != 0) {
            ret = -EIO;
            goto out;
        }
    }

    if (ctx_id == 0) {
        ret = virtio_gpu_ensure_present_context(g, &ctx_id);
        if (ret != 0)
            goto out;
        used_present_ctx = 1;
        if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                        ctx_id, src_resource_id) != 0) {
            ret = -EIO;
            goto out_drop_context;
        }
        src_attached = 1;
    }
    if (virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                                    ctx_id, dst_resource_id) != 0) {
        ret = -EIO;
        goto out_detach_src;
    }
    dst_attached = 1;

    spin_lock(&g->lock);
    src = virtio_gpu_lookup_resource_locked(g, src_resource_id);
    dst = virtio_gpu_lookup_resource_locked(g, dst_resource_id);
    fence_id = ++g->next_fence_id;
    spin_unlock(&g->lock);
    if (src == NULL || dst == NULL) {
        ret = -EINVAL;
        goto out_detach_dst;
    }
    ret = virtio_gpu_submit_present_copy_method(
        g, ctx_id, dst, dst_x, dst_y, w, h, src, src_x, src_y, fence_id,
        use_copy_transfer, use_blit);

out_detach_dst:
    if (dst_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, dst_resource_id);
out_detach_src:
    if (src_attached)
        (void)virtio_gpu_context_resource(
            g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, src_resource_id);
out_drop_context:
    if (ret != 0 && used_present_ctx)
        virtio_gpu_drop_present_context(g, ctx_id);
out:
    virtio_gpu_op_unlock(g);
    return ret;
}

static void virtio_gpu_copy_fb_rect_to_resource(
    struct virtio_gpu_resource *res, volatile void *fb, uint32 src_pitch,
    uint32 x, uint32 y, uint32 w, uint32 h)
{
    uint8 *dst_base;
    volatile uint8 *src_base;

    if (res == NULL || res->backing == NULL || fb == NULL)
        return;

    dst_base = (uint8 *)res->backing;
    src_base = (volatile uint8 *)fb;
    for (uint32 row = 0; row < h; row++) {
        volatile uint32 *src =
            (volatile uint32 *)(src_base + (uint64)(y + row) * src_pitch +
                                (uint64)x * sizeof(uint32));
        uint32 *dst =
            (uint32 *)(dst_base + (uint64)(y + row) * res->width *
                       sizeof(uint32) + (uint64)x * sizeof(uint32));
        for (uint32 col = 0; col < w; col++)
            dst[col] = src[col];
    }
}

void virtio_gpu_present_fb_rect(volatile void *fb, uint32 src_pitch,
                                uint32 x, uint32 y, uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;
    struct virtio_gpu_resource *target;
    uint32 target_slot = 0;
    int target_is_flip = 0;

    if (!g->initialized || fb == NULL || w == 0 || h == 0)
        return;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_FB_PRESENT);
    res = g->scanout_resource;
    if (res == NULL || !res->attached || res->backing == NULL)
        goto out;
    if (x >= res->width || y >= res->height)
        goto out;
    if ((uint64)x + w > res->width)
        w = res->width - x;
    if ((uint64)y + h > res->height)
        h = res->height - y;
    if (w == 0 || h == 0)
        goto out;

    target = res;
    if (g->bound_scanout_resource_id != res->id) {
        struct virtio_gpu_resource *bound = NULL;
        uint32 bound_id = g->bound_scanout_resource_id;

        spin_lock(&g->lock);
        bound = virtio_gpu_lookup_resource_locked(g, bound_id);
        if (bound == NULL || !bound->attached || bound->backing == NULL ||
            bound->width != res->width || bound->height != res->height ||
            !virtio_gpu_present_flip_slot_for_id(g, bound->id,
                                                 &target_slot)) {
            bound = NULL;
        }
        spin_unlock(&g->lock);

        if (bound != NULL) {
            target = bound;
            target_is_flip = 1;
        } else {
            if (virtio_gpu_set_scanout(g, 0, res, 0, 0, res->width,
                                       res->height) != 0)
                goto out;
            g->bound_scanout_resource_id = res->id;
            virtio_gpu_invalidate_present_flip_slot(g, 0);
            virtio_gpu_invalidate_present_flip_slot(g, 1);
        }
    }

    if ((void *)fb != res->backing ||
        src_pitch != res->width * sizeof(uint32))
        virtio_gpu_copy_fb_rect_to_resource(res, fb, src_pitch, x, y, w, h);
    g->present_base_generation++;
    if (g->present_base_generation == 0)
        g->present_base_generation = 1;

    if (target_is_flip) {
        for (uint32 i = 0; i < 2; i++) {
            struct virtio_gpu_resource *flip = g->present_flip_resource[i];

            if (flip == NULL || flip == target || !g->present_flip_valid[i] ||
                flip->width != res->width || flip->height != res->height)
                continue;
            virtio_gpu_copy_fb_rect_to_resource(flip, fb, src_pitch,
                                                x, y, w, h);
            if (virtio_gpu_resource_transfer_scanout(g, flip, x, y, w, h) != 0) {
                virtio_gpu_invalidate_present_flip_slot(g, i);
            } else {
                g->present_flip_base_generation[i] =
                    g->present_base_generation;
            }
        }
        virtio_gpu_copy_fb_rect_to_resource(target, fb, src_pitch, x, y, w, h);
        g->present_flip_base_generation[target_slot] =
            g->present_base_generation;
    }

    if (virtio_gpu_resource_transfer_scanout(g, target, x, y, w, h) != 0)
        goto out;
    virtio_gpu_resource_flush(g, target, x, y, w, h);

out:
    virtio_gpu_op_unlock(g);
}

int virtio_gpu_read_current_scanout(uint32 x, uint32 y, uint32 w, uint32 h,
                                    void *dst, uint32 dst_pitch,
                                    uint32 *screen_width,
                                    uint32 *screen_height,
                                    uint32 *screen_pitch)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res = NULL;
    struct virtio_gpu_resource *present = NULL;
    uint32 src_pitch;
    uint8 *src_base;
    uint8 *dst_base = (uint8 *)dst;
    uint32 present_resource_id = 0;
    uint32 present_ctx_id = 0;
    uint32 present_src_x = 0;
    uint32 present_src_y = 0;
    uint32 present_dst_x = 0;
    uint32 present_dst_y = 0;
    uint32 present_w = 0;
    uint32 present_h = 0;
    uint32 read_ctx_id = 0;
    int ret = 0;

    if (dst == NULL || w == 0 || h == 0 || dst_pitch < w * sizeof(uint32))
        return -EINVAL;
    if (!g->initialized)
        return -ENODEV;

    virtio_gpu_op_lock(g, VIRTIO_GPU_OP_PROBE);
    spin_lock(&g->lock);
    res = g->scanout_resource;
    present_resource_id = g->diagnostic_present_resource_id;
    present_ctx_id = g->diagnostic_present_ctx_id;
    present_src_x = g->diagnostic_present_src_x;
    present_src_y = g->diagnostic_present_src_y;
    present_dst_x = g->diagnostic_present_dst_x;
    present_dst_y = g->diagnostic_present_dst_y;
    present_w = g->diagnostic_present_w;
    present_h = g->diagnostic_present_h;
    spin_unlock(&g->lock);

    if (res == NULL || !res->attached ||
        (res->backing == NULL &&
         (res->pages == NULL || res->npages == 0)) ||
        res->width == 0 || res->height == 0 ||
        x > res->width || w > res->width - x ||
        y > res->height || h > res->height - y) {
        ret = -EINVAL;
        goto out;
    }

    src_pitch = res->width * sizeof(uint32);
    if (res->backing != NULL &&
        (uint64)src_pitch * res->height > res->backing_len) {
        ret = -EINVAL;
        goto out;
    }

    if (virtio_gpu_async_pending(g) &&
        virtio_gpu_drain_async_submit(g, 1) != 0) {
        ret = -EIO;
        goto out;
    }

    if (res->is_3d) {
        struct virtio_gpu_transfer_host_3d *transfer =
            (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
        struct virtio_gpu_ctrl_hdr *resp =
            (struct virtio_gpu_ctrl_hdr *)g->resp_page;

        read_ctx_id = res->ctx_id;
        if (read_ctx_id == 0) {
            ret = virtio_gpu_ensure_present_context(g, &read_ctx_id);
            if (ret != 0)
                goto out;
            ret = virtio_gpu_context_resource(
                g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, read_ctx_id, res->id);
            if (ret != 0) {
                ret = -EIO;
                goto out;
            }
        }
        memset(transfer, 0, sizeof(*transfer));
        transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
        transfer->hdr.ctx_id = read_ctx_id;
        transfer->box.x = x;
        transfer->box.y = y;
        transfer->box.z = 0;
        transfer->box.w = w;
        transfer->box.h = h;
        transfer->box.d = 1;
        transfer->offset = (uint64)y * src_pitch +
                           (uint64)x * sizeof(uint32);
        transfer->resource_id = res->id;
        transfer->level = 0;
        transfer->stride = src_pitch;
        transfer->layer_stride = (uint64)src_pitch * res->height;
        if (virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0,
                              false, resp, sizeof(*resp),
                              VIRTIO_GPU_RESP_OK_NODATA) != 0) {
            ret = -EIO;
            goto out;
        }
    }

    if (res->backing != NULL) {
        src_base = (uint8 *)res->backing;
        for (uint32 row = 0; row < h; row++) {
            memcpy(dst_base + (uint64)row * dst_pitch,
                   src_base + (uint64)(y + row) * src_pitch +
                       (uint64)x * sizeof(uint32),
                   (size_t)w * sizeof(uint32));
        }
    } else {
        for (uint32 row = 0; row < h; row++) {
            uint32 *dst_row =
                (uint32 *)(dst_base + (uint64)row * dst_pitch);

            for (uint32 col = 0; col < w; col++)
                dst_row[col] =
                    virtio_gpu_resource_sample_pixel(res, x + col, y + row);
        }
    }

    spin_lock(&g->lock);
    present = virtio_gpu_lookup_resource_locked(g, present_resource_id);
    spin_unlock(&g->lock);
    if (present != NULL && present->attached &&
        (present->backing != NULL ||
         (present->pages != NULL && present->npages != 0)) &&
        present_w != 0 && present_h != 0 &&
        present_src_x <= present->width &&
        present_w <= present->width - present_src_x &&
        present_src_y <= present->height &&
        present_h <= present->height - present_src_y &&
        present_dst_x <= res->width &&
        present_w <= res->width - present_dst_x &&
        present_dst_y <= res->height &&
        present_h <= res->height - present_dst_y) {
        uint32 ix0 = x > present_dst_x ? x : present_dst_x;
        uint32 iy0 = y > present_dst_y ? y : present_dst_y;
        uint32 req_x1 = x + w;
        uint32 req_y1 = y + h;
        uint32 pres_x1 = present_dst_x + present_w;
        uint32 pres_y1 = present_dst_y + present_h;
        uint32 ix1 = req_x1 < pres_x1 ? req_x1 : pres_x1;
        uint32 iy1 = req_y1 < pres_y1 ? req_y1 : pres_y1;

        if (ix1 > ix0 && iy1 > iy0) {
            struct virtio_gpu_transfer_host_3d *transfer =
                (struct virtio_gpu_transfer_host_3d *)g->cmd_page;
            struct virtio_gpu_ctrl_hdr *resp =
                (struct virtio_gpu_ctrl_hdr *)g->resp_page;
            uint32 present_pitch = present->width * sizeof(uint32);
            uint32 src_start_x;
            uint32 src_start_y;

            if (present_ctx_id == 0) {
                ret = virtio_gpu_ensure_present_context(g, &present_ctx_id);
                if (ret != 0)
                    goto out;
                ret = virtio_gpu_context_resource(
                    g, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                    present_ctx_id, present->id);
                if (ret != 0) {
                    ret = -EIO;
                    goto out;
                }
            }
            memset(transfer, 0, sizeof(*transfer));
            transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
            transfer->hdr.ctx_id = present_ctx_id;
            src_start_x = present_src_x + (ix0 - present_dst_x);
            src_start_y = present_src_y + (iy0 - present_dst_y);
            transfer->box.x = src_start_x;
            transfer->box.y = src_start_y;
            transfer->box.z = 0;
            transfer->box.w = ix1 - ix0;
            transfer->box.h = iy1 - iy0;
            transfer->box.d = 1;
            transfer->offset = (uint64)transfer->box.y * present_pitch +
                               (uint64)transfer->box.x * sizeof(uint32);
            transfer->resource_id = present->id;
            transfer->level = 0;
            transfer->stride = present_pitch;
            transfer->layer_stride = (uint64)present_pitch * present->height;
            if (virtio_gpu_submit(g, transfer, sizeof(*transfer), NULL, 0,
                                  false, resp, sizeof(*resp),
                                  VIRTIO_GPU_RESP_OK_NODATA) != 0) {
                ret = -EIO;
                goto out;
            }
            for (uint32 row = 0; row < iy1 - iy0; row++) {
                uint32 *dst_row =
                    (uint32 *)(dst_base +
                               (uint64)(iy0 - y + row) * dst_pitch +
                               (uint64)(ix0 - x) * sizeof(uint32));

                for (uint32 col = 0; col < ix1 - ix0; col++)
                    dst_row[col] = virtio_gpu_resource_sample_pixel(
                        present, src_start_x + col, src_start_y + row);
            }
        }
    }

    if (screen_width != NULL)
        *screen_width = res->width;
    if (screen_height != NULL)
        *screen_height = res->height;
    if (screen_pitch != NULL)
        *screen_pitch = src_pitch;

out:
    virtio_gpu_op_unlock(g);
    return ret;
}

#else

void virtio_gpu_init(void) {}
void virtio_gpu_get_fb_stats(struct fb_gpu_stats *stats) { (void)stats; }
int virtio_gpu_has_virgl(void) { return 0; }
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
                                   uint32 capset_id, const char *name,
                                   uint32 *ctx_id)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)capset_id;
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
