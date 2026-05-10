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
#define VIRTIO_GPU_IRQ_WAIT_MS 5000

#define VIRTIO_GPU_F_VIRGL          0
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
#define VIRTIO_GPU_CMD_CTX_CREATE         0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY        0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D  0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D 0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D          0x0207
#define VIRTIO_GPU_RESP_OK_NODATA       0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO  0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET       0x1103

#define VIRTIO_GPU_FLAG_FENCE  (1 << 0)
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_CAPSET_VIRGL          1
#define VIRTIO_GPU_CAPSET_VIRGL2         2
#define VIRTIO_GPU_CAPSET_DRM            6
#define VIRTIO_GPU_SMOKE_WIDTH 32
#define VIRTIO_GPU_SMOKE_HEIGHT 32
#define VIRTIO_GPU_MAX_RESOURCES 4096
#define VIRTIO_GPU_MAX_CONTEXTS 64
#define VIRTIO_GPU_MAX_CAPSETS 8

#define VIRGL_CCMD_NOP 0
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))

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
    int attached;
    uint32 ctx_id;
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
    uint32 virgl_capset_id;
    uint32 virgl_capset_version;
    uint32 virgl_capset_size;
    struct virtio_gpu_capset capsets[VIRTIO_GPU_MAX_CAPSETS];
    uint32 next_context_id;
    struct virtio_gpu_context contexts[VIRTIO_GPU_MAX_CONTEXTS];
    uint64 next_fence_id;
    void *cmd_page;
    void *resp_page;
    void *data_page;
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

static void virtio_gpu_mark_all_contexts_failed_locked(struct virtio_gpu *g);

static int virtio_gpu_complete_pending_locked(struct virtio_gpu_queue *q)
{
    if (q->pending_completion != NULL && q->used->idx != q->used_idx) {
        complete_all(q->pending_completion);
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

static int virtio_gpu_submit(struct virtio_gpu *g, void *cmd, uint32 cmd_len,
                             void *data, uint32 data_len, bool data_write,
                             void *resp, uint32 resp_len, uint32 expected)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    uint32 type = ((struct virtio_gpu_ctrl_hdr *)cmd)->type;
    struct virtio_gpu_ctrl_hdr *resp_hdr = resp;
    completion_t done;

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

    if (wait_for_completion_timeout(&done, VIRTIO_GPU_IRQ_WAIT_MS) == 0) {
        virtio_gpu_count_poll_fallback(g);
        for (int i = 0; i < VIRTIO_GPU_POLL_LIMIT; i++) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (q->used->idx != q->used_idx)
                break;
        }
    }

    intena = spin_lock_irqsave(&q->lock);

    if (q->used->idx == q->used_idx) {
        q->pending_completion = NULL;
        spin_unlock_irqrestore(&q->lock, intena);
        virtio_gpu_count_timeout(g);
        virtio_gpu_count_failure(g);
        spin_lock(&g->lock);
        virtio_gpu_mark_all_contexts_failed_locked(g);
        spin_unlock(&g->lock);
        printf("virtio_gpu: command 0x%x timed out\n", type);
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

static void virtio_gpu_mark_all_contexts_failed_locked(struct virtio_gpu *g)
{
    for (int i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        struct virtio_gpu_context *ctx = &g->contexts[i];

        if (!ctx->in_use || ctx->failed)
            continue;
        ctx->failed = 1;
        g->stats.context_failed++;
        g->stats.context_failures++;
    }
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

static int virtio_gpu_submit_display_info(struct virtio_gpu *g)
{
    struct virtio_gpu_ctrl_hdr *cmd =
        (struct virtio_gpu_ctrl_hdr *)g->cmd_page;
    struct virtio_gpu_resp_display_info *resp =
        (struct virtio_gpu_resp_display_info *)g->resp_page;

    memset(cmd, 0, sizeof(*cmd));
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    if (virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                          sizeof(*resp),
                          VIRTIO_GPU_RESP_OK_DISPLAY_INFO) != 0)
        return -1;

    printf("virtio_gpu: display info ok");
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (!resp->pmodes[i].enabled)
            continue;
        if (g->scanout_width == 0 || g->scanout_height == 0) {
            g->scanout_width = resp->pmodes[i].r.width;
            g->scanout_height = resp->pmodes[i].r.height;
        }
        printf(" scanout%d=%ux%u+%u+%u", i, resp->pmodes[i].r.width,
               resp->pmodes[i].r.height, resp->pmodes[i].r.x,
               resp->pmodes[i].r.y);
    }
    printf("\n");
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

    mutex_lock(&g->op_lock);
    spin_lock(&g->lock);
    if (capset_id == 0)
        capset_id = g->virgl_capset_id;
    capset = virtio_gpu_lookup_capset_locked(g, capset_id);
    if (capset == NULL) {
        spin_unlock(&g->lock);
        mutex_unlock(&g->op_lock);
        return -EINVAL;
    }
    ctx = virtio_gpu_alloc_context_slot_locked(g);
    if (ctx == NULL) {
        g->stats.failures++;
        spin_unlock(&g->lock);
        mutex_unlock(&g->op_lock);
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
        mutex_unlock(&g->op_lock);
        return -EIO;
    }
    mutex_unlock(&g->op_lock);

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

    mutex_lock(&g->op_lock);
    spin_lock(&g->lock);
    ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
    if (ctx == NULL) {
        spin_unlock(&g->lock);
        mutex_unlock(&g->op_lock);
        return -ENOENT;
    }
    if (!virtio_gpu_owner_matches(ctx->owner_id, ctx->owner_tgid,
                                  owner_id, owner_tgid)) {
        spin_unlock(&g->lock);
        mutex_unlock(&g->op_lock);
        return -EPERM;
    }
    spin_unlock(&g->lock);

    ret = virtio_gpu_destroy_context(g, ctx_id);
    if (ret == 0) {
        spin_lock(&g->lock);
        ctx = virtio_gpu_lookup_context_locked(g, ctx_id);
        if (ctx != NULL) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    mutex_unlock(&g->op_lock);
    return ret == 0 ? 0 : -EIO;
}

int virtio_gpu_user_submit(uint64 owner_id, pid_t owner_tgid, uint32 ctx_id,
                           uint32 flags, const uint32 *cmds,
                           uint32 nr_dwords, uint64 *fence, uint64 *signaled)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_context *ctx;
    uint64 fence_id;
    int ret;

    if ((flags & ~FB_GPU_VIRGL_SUBMIT_FORCE_FAIL) != 0 ||
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

    mutex_lock(&g->op_lock);
    ret = virtio_gpu_submit_3d(g, ctx_id, cmds, nr_dwords, fence_id);
    mutex_unlock(&g->op_lock);
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
    spin_unlock(&g->lock);
    if (fence)
        *fence = fence_id;
    return 0;
}

int virtio_gpu_user_fence(uint64 wait_for, int wait, uint64 *signaled)
{
    struct virtio_gpu *g = &gpu;
    uint64 done;

    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    spin_lock(&g->lock);
    done = g->stats.last_fence;
    spin_unlock(&g->lock);

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

    mutex_lock(&g->op_lock);
    ret = virtio_gpu_submit_capset(g, id, version, transfer_size, buf);
    mutex_unlock(&g->op_lock);
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

    mutex_lock(&g->op_lock);
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
    mutex_unlock(&g->op_lock);
    return 0;

out_detach:
    if (attached_to_ctx)
        virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    req->ctx_id, res->id);
out_unref:
    if (res != NULL)
        virtio_gpu_resource_unref(g, res);
out:
    mutex_unlock(&g->op_lock);
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

    if (ctx_id != 0)
        virtio_gpu_context_resource(g, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                                    ctx_id, resource_id);
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

    mutex_lock(&g->op_lock);
    ret = virtio_gpu_destroy_resource_locked(g, resource_id, owner_id,
                                             owner_tgid);
    mutex_unlock(&g->op_lock);
    return ret;
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

    mutex_lock(&g->op_lock);
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
        ctx = virtio_gpu_lookup_context_locked(g, context_ids[i]);
        if (ctx != NULL && ctx->owner_tgid == owner_tgid) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    mutex_unlock(&g->op_lock);
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

    mutex_lock(&g->op_lock);
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
        ctx = virtio_gpu_lookup_context_locked(g, context_ids[i]);
        if (ctx != NULL && ctx->owner_id == owner_id) {
            if (ctx->failed && g->stats.context_failed > 0)
                g->stats.context_failed--;
            memset(ctx, 0, sizeof(*ctx));
        }
        spin_unlock(&g->lock);
    }
    mutex_unlock(&g->op_lock);
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
    int ret;

    if (req == NULL || req->resource_id == 0 || req->flags != 0 ||
        req->w == 0 || req->h == 0 || req->d == 0)
        return -EINVAL;
    if (!g->initialized || !g->virgl_capset_id ||
        !(g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL)))
        return -ENODEV;

    mutex_lock(&g->op_lock);
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
    }
    spin_unlock(&g->lock);
    if (res == NULL) {
        mutex_unlock(&g->op_lock);
        return -EINVAL;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = cmd_type;
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

    ret = virtio_gpu_submit(g, cmd, sizeof(*cmd), NULL, 0, false, resp,
                            sizeof(*resp), VIRTIO_GPU_RESP_OK_NODATA);
    mutex_unlock(&g->op_lock);
    return ret == 0 ? 0 : -EIO;
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

    fb_get_resolution(&fb_w, &fb_h);
    if (fb_w >= 640 && fb_h >= 480) {
        width = fb_w;
        height = fb_h;
        g->scanout_width = width;
        g->scanout_height = height;
        if (reported_width != width || reported_height != height) {
            printf("virtio_gpu: using fb0 mode %ux%u for scanout (device reported %ux%u)\n",
                   width, height, reported_width, reported_height);
        }
    } else if (virtio_gpu_cmdline_video(&video_w, &video_h) == 0) {
        width = video_w;
        height = video_h;
        g->scanout_width = width;
        g->scanout_height = height;
        if (reported_width != width || reported_height != height) {
            printf("virtio_gpu: using cmdline video mode %ux%u for scanout (device reported %ux%u)\n",
                   width, height, reported_width, reported_height);
        }
    } else if (g->scanout_width == 0 || g->scanout_height == 0) {
        printf("virtio_gpu: using default mode %ux%u for scanout fallback\n",
               width, height);
    }

    if (virtio_gpu_resource_create_2d(g, width, height,
                                      VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                      &res) != 0)
        return -1;

    virtio_gpu_fill_scanout_pattern(res);

    if (virtio_gpu_resource_attach_backing(g, res) != 0)
        goto fail;
    if (virtio_gpu_set_scanout(g, 0, res, 0, 0, width, height) != 0)
        goto fail;
    scanout_bound = 1;
    if (virtio_gpu_resource_transfer_2d(g, res, 0, 0, width, height) != 0)
        goto fail;
    if (virtio_gpu_resource_flush(g, res, 0, 0, width, height) != 0)
        goto fail;

    g->scanout_resource = res;
    printf("virtio_gpu: persistent scanout resource=%u size=%ux%u bytes=%u alloc=%u\n",
           res->id, width, height, res->backing_len, res->alloc_len);
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
    if (!vd || !vd->found)
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

    struct virtio_gpu *g = &gpu;
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
    virtio_gpu_smoke_resource(g);
    virtio_gpu_init_persistent_scanout(g);
    if (virtio_gpu_has_virgl())
        fb_gpu_register_virgl_render_node();
}

void virtio_gpu_get_fb_stats(struct fb_gpu_stats *stats)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_stats vg_stats;

    if (!g->initialized)
        return;

    spin_lock(&g->lock);
    vg_stats = g->stats;
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
}

int virtio_gpu_has_virgl(void)
{
    struct virtio_gpu *g = &gpu;

    return g->initialized && g->virgl_capset_id &&
        (g->driver_features0 & (1u << VIRTIO_GPU_F_VIRGL));
}

void virtio_gpu_present_fb_rect(volatile void *fb, uint32 src_pitch,
                                uint32 x, uint32 y, uint32 w, uint32 h)
{
    struct virtio_gpu *g = &gpu;
    struct virtio_gpu_resource *res;

    if (!g->initialized || fb == NULL || w == 0 || h == 0)
        return;

    mutex_lock(&g->op_lock);
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

    if ((void *)fb != res->backing ||
        src_pitch != res->width * sizeof(uint32)) {
        uint8 *dst_base = (uint8 *)res->backing;
        volatile uint8 *src_base = (volatile uint8 *)fb;

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

    if (virtio_gpu_resource_transfer_2d(g, res, x, y, w, h) != 0)
        goto out;
    virtio_gpu_resource_flush(g, res, x, y, w, h);

out:
    mutex_unlock(&g->op_lock);
}

#else

void virtio_gpu_init(void) {}
void virtio_gpu_get_fb_stats(struct fb_gpu_stats *stats) { (void)stats; }
int virtio_gpu_has_virgl(void) { return 0; }
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
                           uint32 nr_dwords, uint64 *fence, uint64 *signaled)
{
    (void)owner_id;
    (void)owner_tgid;
    (void)ctx_id;
    (void)flags;
    (void)cmds;
    (void)nr_dwords;
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
