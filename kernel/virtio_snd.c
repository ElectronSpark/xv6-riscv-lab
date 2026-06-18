// Minimal virtio-sound PCI driver.
//
// This is intentionally playback-only for the first kernel slice.  It backs
// the existing OSS /dev/dsp compatibility device with a real QEMU
// virtio-sound output stream when one is present.

#include "types.h"
#include "param.h"
#include "defs.h"
#include "errno.h"
#include "printf.h"
#include "string.h"
#include "trap.h"
#include "dev/pci.h"
#include "dev/virtio.h"
#include "lock/spinlock.h"
#include "lock/mutex.h"
#include "proc/thread.h"
#include "arch/vm.h"
#include <mm/pgtable.h>
#include <mm/vm.h>
#include <vfs/fcntl.h>

extern pagetable_t kernel_pagetable;
extern void sleep_ms(uint64 ms);

#if defined(__x86_64__) || defined(__i386__)

#define AFMT_U8      0x00000008
#define AFMT_S16_LE  0x00000010
#define AFMT_S32_LE  0x00001000

#define VSND_QUEUE_CONTROL 0
#define VSND_QUEUE_EVENT   1
#define VSND_QUEUE_TX      2
#define VSND_QUEUE_RX      3
#define VSND_NQUEUES       4

#define VSND_QSIZE         16
#define VSND_MAX_EVENTS    8
#define VSND_PERIOD_BYTES  4096
#define VSND_BUFFER_BYTES  (VSND_PERIOD_BYTES * 16)

#define VIRTIO_SND_D_OUTPUT 0

#define VIRTIO_SND_R_PCM_INFO       0x0100
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101
#define VIRTIO_SND_R_PCM_PREPARE    0x0102
#define VIRTIO_SND_R_PCM_RELEASE    0x0103
#define VIRTIO_SND_R_PCM_START      0x0104
#define VIRTIO_SND_R_PCM_STOP       0x0105

#define VIRTIO_SND_S_OK       0x8000
#define VIRTIO_SND_S_IO_ERR   0x8003

#define VIRTIO_SND_PCM_FMT_U8  4
#define VIRTIO_SND_PCM_FMT_S16 5
#define VIRTIO_SND_PCM_FMT_S32 17

#define VIRTIO_SND_PCM_RATE_8000  1
#define VIRTIO_SND_PCM_RATE_16000 3
#define VIRTIO_SND_PCM_RATE_22050 4
#define VIRTIO_SND_PCM_RATE_32000 5
#define VIRTIO_SND_PCM_RATE_44100 6
#define VIRTIO_SND_PCM_RATE_48000 7
#define VIRTIO_SND_PCM_RATE_96000 10

struct virtio_snd_config {
    uint32 jacks;
    uint32 streams;
    uint32 chmaps;
} __attribute__((packed));

struct virtio_snd_hdr {
    uint32 code;
} __attribute__((packed));

struct virtio_snd_query_info {
    struct virtio_snd_hdr hdr;
    uint32 start_id;
    uint32 count;
    uint32 size;
} __attribute__((packed));

struct virtio_snd_info {
    uint32 hda_fn_nid;
} __attribute__((packed));

struct virtio_snd_pcm_info {
    struct virtio_snd_info hdr;
    uint32 features;
    uint64 formats;
    uint64 rates;
    uint8 direction;
    uint8 channels_min;
    uint8 channels_max;
    uint8 padding[5];
} __attribute__((packed));

struct virtio_snd_pcm_hdr {
    struct virtio_snd_hdr hdr;
    uint32 stream_id;
} __attribute__((packed));

struct virtio_snd_pcm_set_params {
    struct virtio_snd_pcm_hdr hdr;
    uint32 buffer_bytes;
    uint32 period_bytes;
    uint32 features;
    uint8 channels;
    uint8 format;
    uint8 rate;
    uint8 padding;
} __attribute__((packed));

struct virtio_snd_pcm_xfer {
    uint32 stream_id;
} __attribute__((packed));

struct virtio_snd_pcm_status {
    uint32 status;
    uint32 latency_bytes;
} __attribute__((packed));

struct virtio_snd_event {
    struct virtio_snd_hdr hdr;
    uint32 data;
} __attribute__((packed));

struct vsnd_queue {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16 size;
    uint16 notify_off;
    uint16 last_used_idx;
    spinlock_t lock;
};

struct vsnd_pcm_info_response {
    struct virtio_snd_hdr status;
    struct virtio_snd_pcm_info info[16];
} __attribute__((packed));

struct vsnd_state {
    int initialized;
    int configured;
    int started;
    struct virtio_pci_state pci;
    volatile struct virtio_snd_config *config;
    struct vsnd_queue q[VSND_NQUEUES];
    struct virtio_snd_event *events;
    uint8 *tx_buf;
    uint32 streams;
    uint32 stream_id;
    uint64 formats;
    uint64 rates;
    uint8 channels_min;
    uint8 channels_max;
    int oss_format;
    int rate;
    int channels;
    uint64 submitted_bytes;
    uint64 completed_bytes;
    uint32 last_latency_bytes;
    spinlock_t lock;
    mutex_t op_lock;
};

static struct vsnd_state vsnd;

static uint64 vsnd_dma_addr(const void *ptr)
{
    uint64 addr = (uint64)ptr;
    if (addr >= (uint64)PA2VA(0))
        return VA2PA(addr);
    return addr;
}

static uint64 vsnd_bar_base(struct virtio_pci_discovery *vd, uint8 bar)
{
    uint64 base = (uint64)(vd->bar[bar] & ~0xFU);
    if ((vd->bar[bar] & 0x6) == 0x4 && bar < 5)
        base |= ((uint64)vd->bar[bar + 1]) << 32;
    return base;
}

static uint64 vsnd_map_mmio_window(uint64 bar, uint32 offset, uint32 length)
{
    if (bar & 0x1)
        panic("virtio_snd: capability uses I/O BAR 0x%lx", bar);

    uint64 target = bar + offset;
    uint64 start = PGROUNDDOWN(target);
    uint64 end = PGROUNDUP(target + (length ? length : 1));
    uint64 size = end - start;
    uint64 map_base;

    vm_wlock(kernel_vm);
    map_base = vm_find_free_range(kernel_vm, size, 0);
    if (map_base == 0) {
        vm_wunlock(kernel_vm);
        panic("virtio_snd: failed to allocate MMIO VA window");
    }

    vma_t *vma = vma_alloc(kernel_vm, map_base, size,
                           PROT_READ | PROT_WRITE | VMA_FLAG_KERNEL);
    vm_wunlock(kernel_vm);
    if (vma == NULL)
        panic("virtio_snd: failed to reserve MMIO VA window");

    for (uint64 page_off = 0; page_off < size; page_off += PGSIZE) {
        uint64 va = map_base + page_off;
        uint64 pa = start + page_off;

        if (arch_vm_map(kernel_pagetable, va, PGSIZE, pa,
                        PTE_R | PTE_W) != 0)
            panic("virtio_snd: failed to map MMIO page pa=0x%lx", pa);
    }

    arch_tlb_flush();
    return map_base + (target - start);
}

static void vsnd_notify(uint16 queue)
{
    struct vsnd_queue *q = &vsnd.q[queue];
    volatile uint16 *notify_addr = (volatile uint16 *)
        ((uint8 *)vsnd.pci.notify_base +
         q->notify_off * vsnd.pci.notify_off_multiplier);
    *notify_addr = queue;
}

static int vsnd_queue_setup(uint16 qi, uint16 desired)
{
    volatile struct virtio_pci_common_cfg *cfg = vsnd.pci.common_cfg;
    struct vsnd_queue *q = &vsnd.q[qi];

    cfg->queue_select = qi;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint16 max = cfg->queue_size;
    if (max == 0)
        return -ENODEV;

    uint16 qsize = desired;
    if (qsize > max)
        qsize = max;
    if (qsize > NUM)
        qsize = NUM;

    q->desc = kalloc();
    q->avail = kalloc();
    q->used = kalloc();
    if (!q->desc || !q->avail || !q->used)
        panic("virtio_snd: kalloc queue");
    memset(q->desc, 0, PGSIZE);
    memset(q->avail, 0, PGSIZE);
    memset(q->used, 0, PGSIZE);

    cfg->queue_size = qsize;
    cfg->queue_desc = (uint64)q->desc;
    cfg->queue_driver = (uint64)q->avail;
    cfg->queue_device = (uint64)q->used;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->queue_enable = 1;

    q->size = qsize;
    q->notify_off = cfg->queue_notify_off;
    q->last_used_idx = 0;
    spin_init(&q->lock, "virtio_sndq");
    return 0;
}

static int vsnd_wait_used(struct vsnd_queue *q, uint16 head, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited++) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (q->last_used_idx != q->used->idx) {
            uint16 slot = q->last_used_idx % q->size;
            uint32 id = q->used->ring[slot].id;
            q->last_used_idx++;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            return id == head ? 0 : -EIO;
        }
        sleep_ms(1);
    }
    return -ETIMEDOUT;
}

static int vsnd_control(void *request, uint32 request_len, void *response,
                        uint32 response_len)
{
    struct vsnd_queue *q = &vsnd.q[VSND_QUEUE_CONTROL];
    uint16 head = 0;

    spin_lock(&q->lock);
    q->desc[0].addr = vsnd_dma_addr(request);
    q->desc[0].len = request_len;
    q->desc[0].flags = VRING_DESC_F_NEXT;
    q->desc[0].next = 1;
    q->desc[1].addr = vsnd_dma_addr(response);
    q->desc[1].len = response_len;
    q->desc[1].flags = VRING_DESC_F_WRITE;
    q->desc[1].next = 0;

    q->avail->ring[q->avail->idx % q->size] = head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    spin_unlock(&q->lock);

    vsnd_notify(VSND_QUEUE_CONTROL);
    return vsnd_wait_used(q, head, 1000);
}

static int vsnd_simple_cmd(uint32 code)
{
    struct virtio_snd_pcm_hdr req;
    struct virtio_snd_hdr resp;

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.hdr.code = code;
    req.stream_id = vsnd.stream_id;
    int ret = vsnd_control(&req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;
    return resp.code == VIRTIO_SND_S_OK ? 0 : -EIO;
}

static int vsnd_oss_format_to_virtio(int oss_format)
{
    switch (oss_format) {
    case AFMT_U8:
        return VIRTIO_SND_PCM_FMT_U8;
    case AFMT_S32_LE:
        return VIRTIO_SND_PCM_FMT_S32;
    case AFMT_S16_LE:
    default:
        return VIRTIO_SND_PCM_FMT_S16;
    }
}

static int vsnd_rate_to_virtio(int rate)
{
    switch (rate) {
    case 8000:
        return VIRTIO_SND_PCM_RATE_8000;
    case 16000:
        return VIRTIO_SND_PCM_RATE_16000;
    case 22050:
        return VIRTIO_SND_PCM_RATE_22050;
    case 32000:
        return VIRTIO_SND_PCM_RATE_32000;
    case 44100:
        return VIRTIO_SND_PCM_RATE_44100;
    case 96000:
        return VIRTIO_SND_PCM_RATE_96000;
    case 48000:
    default:
        return VIRTIO_SND_PCM_RATE_48000;
    }
}

static int vsnd_stream_supports(int oss_format, int rate, int channels)
{
    int vfmt = vsnd_oss_format_to_virtio(oss_format);
    int vrate = vsnd_rate_to_virtio(rate);

    if (((vsnd.formats >> vfmt) & 1ULL) == 0)
        return 0;
    if (((vsnd.rates >> vrate) & 1ULL) == 0)
        return 0;
    if (channels < vsnd.channels_min || channels > vsnd.channels_max)
        return 0;
    return 1;
}

static int vsnd_configure_locked(int oss_format, int rate, int channels)
{
    if (!vsnd.initialized)
        return -ENODEV;
    if (channels <= 0)
        channels = 2;
    if (channels > 2)
        channels = 2;

    if (vsnd.configured && vsnd.oss_format == oss_format &&
        vsnd.rate == rate && vsnd.channels == channels)
        return 0;

    if (!vsnd_stream_supports(oss_format, rate, channels))
        return -EOPNOTSUPP;

    if (vsnd.started) {
        (void)vsnd_simple_cmd(VIRTIO_SND_R_PCM_STOP);
        vsnd.started = 0;
    }
    if (vsnd.configured)
        (void)vsnd_simple_cmd(VIRTIO_SND_R_PCM_RELEASE);

    struct virtio_snd_pcm_set_params req;
    struct virtio_snd_hdr resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    req.hdr.stream_id = vsnd.stream_id;
    req.buffer_bytes = VSND_BUFFER_BYTES;
    req.period_bytes = VSND_PERIOD_BYTES;
    req.channels = (uint8)channels;
    req.format = (uint8)vsnd_oss_format_to_virtio(oss_format);
    req.rate = (uint8)vsnd_rate_to_virtio(rate);

    int ret = vsnd_control(&req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;
    if (resp.code != VIRTIO_SND_S_OK)
        return -EIO;

    ret = vsnd_simple_cmd(VIRTIO_SND_R_PCM_PREPARE);
    if (ret < 0)
        return ret;
    ret = vsnd_simple_cmd(VIRTIO_SND_R_PCM_START);
    if (ret < 0)
        return ret;

    vsnd.configured = 1;
    vsnd.started = 1;
    vsnd.oss_format = oss_format;
    vsnd.rate = rate;
    vsnd.channels = channels;
    spin_lock(&vsnd.lock);
    vsnd.submitted_bytes = 0;
    vsnd.completed_bytes = 0;
    vsnd.last_latency_bytes = 0;
    spin_unlock(&vsnd.lock);
    return 0;
}

static void vsnd_copyin_silence_tail(uint8 *dst, uint32 valid, uint32 total)
{
    if (valid < total)
        memset(dst + valid, 0, total - valid);
}

static int vsnd_tx_period_locked(int user, const void *buf, uint32 count)
{
    struct vsnd_queue *q = &vsnd.q[VSND_QUEUE_TX];
    struct virtio_snd_pcm_xfer xfer;
    struct virtio_snd_pcm_status status;
    uint32 period = count;

    if (period > VSND_PERIOD_BYTES)
        period = VSND_PERIOD_BYTES;
    if (user) {
        if (either_copyin(vsnd.tx_buf, 1, (uint64)buf, period) < 0)
            return -EFAULT;
    } else {
        memcpy(vsnd.tx_buf, buf, period);
    }
    vsnd_copyin_silence_tail(vsnd.tx_buf, period, VSND_PERIOD_BYTES);

    memset(&xfer, 0, sizeof(xfer));
    memset(&status, 0, sizeof(status));
    xfer.stream_id = vsnd.stream_id;

    spin_lock(&q->lock);
    q->desc[0].addr = vsnd_dma_addr(&xfer);
    q->desc[0].len = sizeof(xfer);
    q->desc[0].flags = VRING_DESC_F_NEXT;
    q->desc[0].next = 1;
    q->desc[1].addr = vsnd_dma_addr(vsnd.tx_buf);
    q->desc[1].len = period;
    q->desc[1].flags = VRING_DESC_F_NEXT;
    q->desc[1].next = 2;
    q->desc[2].addr = vsnd_dma_addr(&status);
    q->desc[2].len = sizeof(status);
    q->desc[2].flags = VRING_DESC_F_WRITE;
    q->desc[2].next = 0;
    q->avail->ring[q->avail->idx % q->size] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    spin_lock(&vsnd.lock);
    vsnd.submitted_bytes += period;
    spin_unlock(&vsnd.lock);
    spin_unlock(&q->lock);

    vsnd_notify(VSND_QUEUE_TX);
    int ret = vsnd_wait_used(q, 0, 5000);
    if (ret < 0)
        return ret;
    if (status.status != VIRTIO_SND_S_OK && status.status != 0)
        return -EIO;
    spin_lock(&vsnd.lock);
    vsnd.completed_bytes += period;
    vsnd.last_latency_bytes = status.latency_bytes;
    spin_unlock(&vsnd.lock);
    return (int)period;
}

int virtio_snd_write(int user, const void *buf, size_t count, int oss_format,
                     int rate, int channels, int nonblock)
{
    (void)nonblock;
    if (buf == NULL)
        return -EINVAL;
    if (!vsnd.initialized)
        return -ENODEV;

    mutex_lock(&vsnd.op_lock);
    int ret = vsnd_configure_locked(oss_format, rate, channels);
    if (ret < 0) {
        mutex_unlock(&vsnd.op_lock);
        return ret;
    }

    size_t done = 0;
    while (done < count) {
        uint32 chunk = (uint32)(count - done);
        if (chunk > VSND_PERIOD_BYTES)
            chunk = VSND_PERIOD_BYTES;
        ret = vsnd_tx_period_locked(user, (const uint8 *)buf + done, chunk);
        if (ret < 0) {
            mutex_unlock(&vsnd.op_lock);
            return done ? (int)done : ret;
        }
        done += (size_t)ret;
    }
    mutex_unlock(&vsnd.op_lock);
    return (int)done;
}

void virtio_snd_reset(void)
{
    if (!vsnd.initialized)
        return;
    mutex_lock(&vsnd.op_lock);
    if (vsnd.started) {
        (void)vsnd_simple_cmd(VIRTIO_SND_R_PCM_STOP);
        vsnd.started = 0;
    }
    if (vsnd.configured) {
        (void)vsnd_simple_cmd(VIRTIO_SND_R_PCM_RELEASE);
        vsnd.configured = 0;
    }
    spin_lock(&vsnd.lock);
    vsnd.submitted_bytes = 0;
    vsnd.completed_bytes = 0;
    vsnd.last_latency_bytes = 0;
    spin_unlock(&vsnd.lock);
    mutex_unlock(&vsnd.op_lock);
}

void virtio_snd_drain(void)
{
    while (virtio_snd_pending_bytes() != 0)
        sleep_ms(1);
}

uint64 virtio_snd_pending_bytes(void)
{
    if (!vsnd.initialized)
        return 0;
    spin_lock(&vsnd.lock);
    uint64 pending = vsnd.submitted_bytes - vsnd.completed_bytes;
    if (vsnd.last_latency_bytes > pending)
        pending = vsnd.last_latency_bytes;
    spin_unlock(&vsnd.lock);
    return pending;
}

uint64 virtio_snd_free_bytes(void)
{
    uint64 pending = virtio_snd_pending_bytes();
    if (pending >= VSND_BUFFER_BYTES)
        return 0;
    return VSND_BUFFER_BYTES - pending;
}

uint64 virtio_snd_played_bytes(void)
{
    if (!vsnd.initialized)
        return 0;
    spin_lock(&vsnd.lock);
    uint64 played = vsnd.completed_bytes;
    spin_unlock(&vsnd.lock);
    return played;
}

int virtio_snd_available(void)
{
    return vsnd.initialized;
}

int virtio_snd_supported_oss_formats(void)
{
    int formats = 0;
    if (!vsnd.initialized)
        return 0;
    if ((vsnd.formats >> VIRTIO_SND_PCM_FMT_U8) & 1ULL)
        formats |= AFMT_U8;
    if ((vsnd.formats >> VIRTIO_SND_PCM_FMT_S16) & 1ULL)
        formats |= AFMT_S16_LE;
    if ((vsnd.formats >> VIRTIO_SND_PCM_FMT_S32) & 1ULL)
        formats |= AFMT_S32_LE;
    return formats;
}

const char *virtio_snd_backend_name(void)
{
    return vsnd.initialized ? "virtio-sound" : "timer";
}

static void vsnd_post_event(uint16 id)
{
    struct vsnd_queue *q = &vsnd.q[VSND_QUEUE_EVENT];

    q->desc[id].addr = vsnd_dma_addr(&vsnd.events[id]);
    q->desc[id].len = sizeof(vsnd.events[id]);
    q->desc[id].flags = VRING_DESC_F_WRITE;
    q->desc[id].next = 0;
    q->avail->ring[q->avail->idx % q->size] = id;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void vsnd_drain_events(void)
{
    struct vsnd_queue *q = &vsnd.q[VSND_QUEUE_EVENT];

    spin_lock(&q->lock);
    while (q->last_used_idx != q->used->idx) {
        uint16 slot = q->last_used_idx % q->size;
        uint32 id = q->used->ring[slot].id;
        q->last_used_idx++;
        if (id < VSND_MAX_EVENTS)
            vsnd_post_event((uint16)id);
    }
    spin_unlock(&q->lock);
    vsnd_notify(VSND_QUEUE_EVENT);
}

static void virtio_snd_intr(int irq, void *data, device_t *dev)
{
    (void)irq;
    (void)data;
    (void)dev;
    if (!vsnd.initialized)
        return;
    if (vsnd.pci.isr != NULL) {
        volatile uint8 isr_status = *vsnd.pci.isr;
        (void)isr_status;
    }
    vsnd_drain_events();
}

static int vsnd_query_streams(void)
{
    struct virtio_snd_query_info req;
    struct vsnd_pcm_info_response resp;

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.hdr.code = VIRTIO_SND_R_PCM_INFO;
    req.start_id = 0;
    req.count = vsnd.streams;
    if (req.count > 16)
        req.count = 16;
    req.size = sizeof(struct virtio_snd_pcm_info);

    int ret = vsnd_control(&req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;
    if (resp.status.code != VIRTIO_SND_S_OK)
        return -EIO;

    for (uint32 i = 0; i < req.count; i++) {
        struct virtio_snd_pcm_info *info = &resp.info[i];
        if (info->direction != VIRTIO_SND_D_OUTPUT)
            continue;
        vsnd.stream_id = i;
        vsnd.formats = info->formats;
        vsnd.rates = info->rates;
        vsnd.channels_min = info->channels_min;
        vsnd.channels_max = info->channels_max;
        if (vsnd.channels_min == 0)
            vsnd.channels_min = 1;
        if (vsnd.channels_max == 0)
            vsnd.channels_max = 2;
        return 0;
    }
    return -ENODEV;
}

void virtio_snd_init(void)
{
    struct virtio_pci_discovery *vd = pci_get_virtio_snd(0);
    if (!vd || !vd->found)
        return;

    if (!vd->common_cfg_cap || !vd->notify_cfg_cap || !vd->isr_cfg_cap ||
        !vd->device_cfg_cap) {
        printf("virtio_snd: missing PCI capability, skipping driver init\n");
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

    memset(&vsnd, 0, sizeof(vsnd));
    spin_init(&vsnd.lock, "virtio_snd");
    mutex_init(&vsnd.op_lock, "virtio_snd_op");
    vsnd.pci.use_pci = 1;
    vsnd.pci.common_cfg = (volatile struct virtio_pci_common_cfg *)
        vsnd_map_mmio_window(vsnd_bar_base(vd, cc_bar), cc_off, cc_len);
    vsnd.pci.notify_base = (volatile uint16 *)
        vsnd_map_mmio_window(vsnd_bar_base(vd, n_bar), n_off, n_len);
    vsnd.pci.notify_off_multiplier = n_mult;
    vsnd.pci.isr = (volatile uint8 *)
        vsnd_map_mmio_window(vsnd_bar_base(vd, i_bar), i_off, i_len);
    vsnd.config = (volatile struct virtio_snd_config *)
        vsnd_map_mmio_window(vsnd_bar_base(vd, d_bar), d_off, d_len);

    volatile struct virtio_pci_common_cfg *cfg = vsnd.pci.common_cfg;
    cfg->device_status = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (cfg->device_status != 0)
        ;

    uint8 status = VIRTIO_CONFIG_S_ACKNOWLEDGE;
    cfg->device_status = status;
    status |= VIRTIO_CONFIG_S_DRIVER;
    cfg->device_status = status;

    cfg->device_feature_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32 devfeat1 = cfg->device_feature;
    cfg->driver_feature_select = 0;
    cfg->driver_feature = 0;
    cfg->driver_feature_select = 1;
    cfg->driver_feature =
        (devfeat1 & (1U << (VIRTIO_F_VERSION_1 - 32))) ?
        (1U << (VIRTIO_F_VERSION_1 - 32)) : 0;

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(cfg->device_status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        printf("virtio_snd: device rejected feature set\n");
        cfg->device_status = 0;
        return;
    }

    vsnd.streams = vsnd.config->streams;
    if (cfg->num_queues < VSND_NQUEUES || vsnd.streams == 0) {
        printf("virtio_snd: unsupported config queues=%u streams=%u\n",
               cfg->num_queues, vsnd.streams);
        cfg->device_status = 0;
        return;
    }

    if (vsnd_queue_setup(VSND_QUEUE_CONTROL, VSND_QSIZE) < 0 ||
        vsnd_queue_setup(VSND_QUEUE_EVENT, VSND_MAX_EVENTS) < 0 ||
        vsnd_queue_setup(VSND_QUEUE_TX, VSND_QSIZE) < 0 ||
        vsnd_queue_setup(VSND_QUEUE_RX, VSND_QSIZE) < 0) {
        printf("virtio_snd: queue setup failed\n");
        cfg->device_status = 0;
        return;
    }

    vsnd.events = kalloc();
    vsnd.tx_buf = kalloc();
    if (!vsnd.events || !vsnd.tx_buf)
        panic("virtio_snd: kalloc buffers");
    memset(vsnd.events, 0, PGSIZE);
    memset(vsnd.tx_buf, 0, PGSIZE);
    for (uint16 i = 0; i < VSND_MAX_EVENTS; i++)
        vsnd_post_event(i);

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (vsnd_query_streams() < 0) {
        printf("virtio_snd: no usable output PCM stream\n");
        cfg->device_status = 0;
        return;
    }

    struct irq_desc irq_desc = {
        .handler = virtio_snd_intr,
        .data = &vsnd,
        .dev = NULL,
    };
    int irq_ret = register_irq_handler(PLIC_IRQ(vd->irq_line), &irq_desc);
    if (irq_ret == 0) {
        extern void plic_enable_irq_level(int irq);
        plic_enable_irq_level(vd->irq_line);
    } else {
        printf("virtio_snd: WARNING: IRQ %d registration failed (%d)\n",
               vd->irq_line, irq_ret);
    }

    vsnd.initialized = 1;
    vsnd_notify(VSND_QUEUE_EVENT);
    printf("virtio_snd: initialized streams=%u output=%u formats=0x%lx rates=0x%lx channels=%u..%u irq=%d\n",
           vsnd.streams, vsnd.stream_id, vsnd.formats, vsnd.rates,
           vsnd.channels_min, vsnd.channels_max, vd->irq_line);
}

#else

void virtio_snd_init(void) {}
int virtio_snd_available(void) { return 0; }
int virtio_snd_write(int user, const void *buf, size_t count, int oss_format,
                     int rate, int channels, int nonblock)
{
    (void)user;
    (void)buf;
    (void)count;
    (void)oss_format;
    (void)rate;
    (void)channels;
    (void)nonblock;
    return -ENODEV;
}
void virtio_snd_reset(void) {}
void virtio_snd_drain(void) {}
uint64 virtio_snd_free_bytes(void) { return 0; }
uint64 virtio_snd_pending_bytes(void) { return 0; }
uint64 virtio_snd_played_bytes(void) { return 0; }
int virtio_snd_supported_oss_formats(void) { return 0; }
const char *virtio_snd_backend_name(void) { return "timer"; }

#endif
