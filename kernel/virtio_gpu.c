//
// Minimal virtio-gpu PCI transport bring-up.
//
// This does not replace /dev/fb0 yet: Bochs BGA remains the active scanout
// path. The driver initializes a modern virtio-gpu device, negotiates a basic
// feature set, brings up queue 0, and issues GET_DISPLAY_INFO so later stages
// can attach resource and transfer commands to a known-good transport.
//

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "dev/fb.h"
#include "dev/virtio.h"
#include "dev/pci.h"
#include <mm/pgtable.h>
#include <mm/page.h>
#include <mm/vm.h>
#include "arch/vm.h"

#if defined(__x86_64__) || defined(__i386__)

#define VIRTIO_GPU_MAX_SCANOUTS 16
#define VIRTIO_GPU_POLL_LIMIT 10000000

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF     0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT        0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH     0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_RESP_OK_NODATA       0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_SMOKE_WIDTH 32
#define VIRTIO_GPU_SMOKE_HEIGHT 32
#define VIRTIO_GPU_MAX_RESOURCES 16

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

struct virtio_gpu_queue {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16 size;
    uint16 used_idx;
    uint16 notify_off;
    spinlock_t lock;
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
};

struct virtio_gpu_resource {
    int in_use;
    uint32 id;
    uint32 width;
    uint32 height;
    uint32 format;
    uint32 backing_len;
    uint32 alloc_len;
    uint32 backing_order;
    void *backing;
    int attached;
};

struct virtio_gpu {
    int initialized;
    struct virtio_pci_state pci;
    volatile struct virtio_gpu_config *config;
    struct virtio_gpu_queue ctrlq;
    spinlock_t lock;
    uint32 next_resource_id;
    struct virtio_gpu_resource resources[VIRTIO_GPU_MAX_RESOURCES];
    struct virtio_gpu_resource *scanout_resource;
    struct virtio_gpu_stats stats;
    uint32 scanout_width;
    uint32 scanout_height;
    void *cmd_page;
    void *resp_page;
    void *data_page;
};

static struct virtio_gpu gpu;

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

static void virtio_gpu_count_command(struct virtio_gpu *g, uint32 type)
{
    spin_lock(&g->lock);
    g->stats.commands++;
    if (type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D)
        g->stats.transfers++;
    else if (type == VIRTIO_GPU_CMD_RESOURCE_FLUSH)
        g->stats.flushes++;
    else if (type == VIRTIO_GPU_CMD_SET_SCANOUT)
        g->stats.scanouts++;
    spin_unlock(&g->lock);
}

static int virtio_gpu_submit(struct virtio_gpu *g, void *cmd, uint32 cmd_len,
                             void *data, uint32 data_len, bool data_write,
                             void *resp, uint32 resp_len, uint32 expected)
{
    struct virtio_gpu_queue *q = &g->ctrlq;
    uint32 type = ((struct virtio_gpu_ctrl_hdr *)cmd)->type;
    struct virtio_gpu_ctrl_hdr *resp_hdr = resp;

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
    virtio_gpu_notify(g, 0);

    for (int i = 0; i < VIRTIO_GPU_POLL_LIMIT; i++) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (q->used->idx != q->used_idx)
            break;
    }

    if (q->used->idx == q->used_idx) {
        spin_unlock_irqrestore(&q->lock, intena);
        virtio_gpu_count_timeout(g);
        virtio_gpu_count_failure(g);
        printf("virtio_gpu: command 0x%x timed out\n", type);
        return -1;
    }

    q->used_idx = q->used->idx;
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
    page_free(backing, backing_order);
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

static int virtio_gpu_init_persistent_scanout(struct virtio_gpu *g)
{
    struct virtio_gpu_resource *res;
    uint32 width = g->scanout_width ? g->scanout_width : 640;
    uint32 height = g->scanout_height ? g->scanout_height : 480;
    int scanout_bound = 0;

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
    return 0;
}

void virtio_gpu_init(void)
{
    struct virtio_pci_discovery *vd = pci_get_virtio_gpu(0);
    if (!vd || !vd->found)
        return;

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
    g->next_resource_id = 1;
    g->pci.use_pci = 1;
    g->pci.common_cfg = (volatile struct virtio_pci_common_cfg *)cfg_va;
    g->pci.notify_base = (volatile uint16 *)notify_va;
    g->pci.notify_off_multiplier = n_mult;
    g->pci.isr = (volatile uint8 *)isr_va;
    g->config = (volatile struct virtio_gpu_config *)dev_cfg_va;

    volatile struct virtio_pci_common_cfg *cfg = g->pci.common_cfg;

    cfg->device_status = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (cfg->device_status != 0)
        ;

    uint8 status = VIRTIO_CONFIG_S_ACKNOWLEDGE;
    cfg->device_status = status;
    status |= VIRTIO_CONFIG_S_DRIVER;
    cfg->device_status = status;

    cfg->device_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32 features0 = cfg->device_feature;

    cfg->driver_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->driver_feature = 0;

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(cfg->device_status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        printf("virtio_gpu: device rejected feature set\n");
        cfg->device_status = 0;
        return;
    }

    if (virtio_gpu_queue_init(g) != 0) {
        cfg->device_status = 0;
        return;
    }

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    printf("virtio_gpu: initialized queues=%u features0=0x%x scanouts=%u capsets=%u irq=%d\n",
           cfg->num_queues, features0, g->config->num_scanouts,
           g->config->num_capsets, vd->irq_line);

    virtio_gpu_submit_display_info(g);
    virtio_gpu_smoke_resource(g);
    virtio_gpu_init_persistent_scanout(g);
    g->initialized = 1;
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
}

#else

void virtio_gpu_init(void) {}
void virtio_gpu_get_fb_stats(struct fb_gpu_stats *stats) { (void)stats; }

#endif
