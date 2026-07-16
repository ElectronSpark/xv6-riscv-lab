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
#include "proc/sched.h"
#include "arch/vm.h"
#include <mm/pgtable.h>
#include <mm/vm.h>
#include <vfs/fcntl.h>

extern pagetable_t kernel_pagetable;

#if defined(__x86_64__) || defined(__i386__)

#define AFMT_U8      0x00000008
#define AFMT_S16_LE  0x00000010
#define AFMT_S32_LE  0x00001000

#define VSND_QUEUE_CONTROL 0
#define VSND_QUEUE_EVENT   1
#define VSND_QUEUE_TX      2
#define VSND_QUEUE_RX      3
#define VSND_NQUEUES       4

#define VSND_QSIZE         256
#define VSND_MAX_EVENTS    8
#define VSND_PERIOD_BYTES  2048
#define VSND_BUFFER_BYTES  (VSND_PERIOD_BYTES * 64)
#define VSND_ASYNC_BASE    0
#define VSND_ASYNC_DESC_PER_SLOT 3
#define VSND_ASYNC_DEPTH   64
#define VSND_DRAIN_TIMEOUT_MS 10000

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
    /*
     * Software period assembler.  ALSA is allowed to issue writes as small as
     * one frame, while one virtio-sound transfer consumes three descriptors.
     * Keep fragments here until a useful transfer can be submitted so byte
     * ring space cannot be exhausted by descriptor-chain fragmentation.
     */
    uint8 *tx_buf;
    uint32 staged_bytes;
    int staged_start_if_needed;
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
    /* Monotonic device-lifetime total retained across ALSA stream resets. */
    uint64 total_played_bytes;
    uint32 last_latency_bytes;
    uint64 last_latency_ms;
    uint64 play_started_ms;
    int tx_error;
    uint32 tx_error_status;
    uint64 tx_error_count;
    struct virtio_snd_pcm_xfer sync_xfer;
    struct virtio_snd_pcm_status sync_status;
    uint32 sync_bytes;
    int sync_in_flight;
    struct virtio_snd_pcm_xfer async_xfer[VSND_ASYNC_DEPTH];
    struct virtio_snd_pcm_status async_status[VSND_ASYNC_DEPTH];
    uint8 *async_tx_buf[VSND_ASYNC_DEPTH];
    uint32 async_bytes[VSND_ASYNC_DEPTH];
    uint64 async_in_flight_mask;
    uint64 async_retired_mask;
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

static void vsnd_note_async_completion_locked(uint32 id)
{
    uint32 slot;
    uint32 bytes;
    uint32 status = VIRTIO_SND_S_OK;
    int log_error = 0;
    uint64 error_count = 0;

    if (id < VSND_ASYNC_BASE)
        return;
    slot = (id - VSND_ASYNC_BASE) / VSND_ASYNC_DESC_PER_SLOT;
    if (slot >= VSND_ASYNC_DEPTH ||
        VSND_ASYNC_BASE + slot * VSND_ASYNC_DESC_PER_SLOT != id)
        return;

    spin_lock(&vsnd.lock);
    bytes = vsnd.async_bytes[slot];
    if (vsnd.async_in_flight_mask & (1ULL << slot)) {
        status = vsnd.async_status[slot].status;
        if ((vsnd.async_retired_mask & (1ULL << slot)) == 0) {
            vsnd.completed_bytes += bytes;
            vsnd.last_latency_bytes = vsnd.async_status[slot].latency_bytes;
            vsnd.last_latency_ms = sched_timer_now_ms();
        }
        if ((vsnd.async_retired_mask & (1ULL << slot)) == 0 &&
            status != VIRTIO_SND_S_OK) {
            vsnd.tx_error = 1;
            vsnd.tx_error_status = status;
            vsnd.tx_error_count++;
            error_count = vsnd.tx_error_count;
            log_error = 1;
        }
        vsnd.async_in_flight_mask &= ~(1ULL << slot);
        vsnd.async_retired_mask &= ~(1ULL << slot);
        vsnd.async_bytes[slot] = 0;
    }
    spin_unlock(&vsnd.lock);

    if (log_error)
        printf("virtio_snd: tx completion status=0x%x slot=%u bytes=%u errors=%lu\n",
               status, slot, bytes, error_count);
}

static uint32 vsnd_async_slot_limit(void)
{
    uint16 qsize = vsnd.q[VSND_QUEUE_TX].size;
    uint32 limit;

    if (qsize <= VSND_ASYNC_BASE)
        return 0;
    limit = (qsize - VSND_ASYNC_BASE) / VSND_ASYNC_DESC_PER_SLOT;
    if (limit > VSND_ASYNC_DEPTH)
        limit = VSND_ASYNC_DEPTH;
    return limit;
}

static int vsnd_tx_busy_locked(void)
{
    return vsnd.sync_in_flight || vsnd.async_in_flight_mask != 0;
}

static void vsnd_retire_tx_locked(void)
{
    if (vsnd.sync_in_flight) {
        /*
         * The current TX path does not submit sync descriptors, but keep the
         * accounting conservative if that path is reintroduced.
         */
        vsnd.sync_in_flight = 0;
        vsnd.sync_bytes = 0;
    }

    vsnd.async_retired_mask |= vsnd.async_in_flight_mask;
    for (uint32 slot = 0; slot < VSND_ASYNC_DEPTH; slot++) {
        if ((vsnd.async_in_flight_mask & (1ULL << slot)) != 0)
            continue;
        vsnd.async_bytes[slot] = 0;
    }
}

static int vsnd_drain_used_locked(struct vsnd_queue *q, uint16 wanted,
                                  int look_for_wanted)
{
    int found = 0;

    while (q->last_used_idx != q->used->idx) {
        uint16 slot = q->last_used_idx % q->size;
        uint32 id = q->used->ring[slot].id;
        q->last_used_idx++;
        if (q == &vsnd.q[VSND_QUEUE_TX])
            vsnd_note_async_completion_locked(id);
        if (look_for_wanted && id == wanted)
            found = 1;
    }
    return found;
}

static void vsnd_poll_tx_completions(void)
{
    struct vsnd_queue *q = &vsnd.q[VSND_QUEUE_TX];

    if (!vsnd.initialized)
        return;
    spin_lock(&q->lock);
    (void)vsnd_drain_used_locked(q, 0, 0);
    spin_unlock(&q->lock);
}

static int vsnd_wait_used(struct vsnd_queue *q, uint16 head, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited++) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        spin_lock(&q->lock);
        int found = vsnd_drain_used_locked(q, head, 1);
        spin_unlock(&q->lock);
        if (found) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            return 0;
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

static uint64 vsnd_bytes_per_second_locked(void)
{
    uint64 sample_bytes;
    uint64 channels;
    uint64 rate;

    switch (vsnd.oss_format) {
    case AFMT_U8:
        sample_bytes = 1;
        break;
    case AFMT_S32_LE:
        sample_bytes = 4;
        break;
    case AFMT_S16_LE:
    default:
        sample_bytes = 2;
        break;
    }

    channels = vsnd.channels > 0 ? (uint64)vsnd.channels : 2;
    rate = vsnd.rate > 0 ? (uint64)vsnd.rate : 48000;
    return rate * channels * sample_bytes;
}

static uint64 vsnd_paced_played_bytes_locked(void)
{
    uint64 accepted;

    accepted = vsnd.completed_bytes;
    if (accepted > vsnd.submitted_bytes)
        accepted = vsnd.submitted_bytes;

    /* Playback descriptors may be queued before PCM_START, as Linux does. */
    if (vsnd.play_started_ms == 0)
        return 0;

    uint64 elapsed_ms = sched_timer_now_ms() - vsnd.play_started_ms;
    uint64 elapsed_bytes = (vsnd_bytes_per_second_locked() * elapsed_ms) /
                           1000;

    if (elapsed_bytes > vsnd.submitted_bytes)
        elapsed_bytes = vsnd.submitted_bytes;
    return accepted < elapsed_bytes ? accepted : elapsed_bytes;
}

static void vsnd_roll_played_total_locked(void)
{
    vsnd.total_played_bytes += vsnd_paced_played_bytes_locked();
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

    vsnd_poll_tx_completions();
    spin_lock(&vsnd.lock);
    int tx_busy = vsnd_tx_busy_locked();
    spin_unlock(&vsnd.lock);
    if (tx_busy)
        return -EAGAIN;

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

    vsnd.configured = 1;
    vsnd.started = 0;
    spin_lock(&vsnd.lock);
    /* Roll the previous stream using its previous format/rate. */
    vsnd_roll_played_total_locked();
    vsnd.oss_format = oss_format;
    vsnd.rate = rate;
    vsnd.channels = channels;
    vsnd.submitted_bytes = 0;
    vsnd.completed_bytes = 0;
    vsnd.last_latency_bytes = 0;
    vsnd.last_latency_ms = 0;
    vsnd.play_started_ms = 0;
    vsnd.tx_error = 0;
    vsnd.tx_error_status = 0;
    vsnd.sync_in_flight = 0;
    vsnd.sync_bytes = 0;
    vsnd.staged_bytes = 0;
    vsnd.staged_start_if_needed = 0;
    vsnd.async_in_flight_mask = 0;
    vsnd.async_retired_mask = 0;
    memset(vsnd.async_bytes, 0, sizeof(vsnd.async_bytes));
    spin_unlock(&vsnd.lock);
    return 0;
}

static void vsnd_tx_timeout_recover_locked(struct vsnd_queue *q)
{
    (void)q;

    if (vsnd.started) {
        (void)vsnd_simple_cmd(VIRTIO_SND_R_PCM_STOP);
        vsnd.started = 0;
    }
    if (vsnd.configured) {
        (void)vsnd_simple_cmd(VIRTIO_SND_R_PCM_RELEASE);
        vsnd.configured = 0;
    }

    spin_lock(&vsnd.lock);
    vsnd_roll_played_total_locked();
    vsnd.last_latency_bytes = 0;
    vsnd.last_latency_ms = 0;
    vsnd.play_started_ms = 0;
    vsnd.tx_error = 1;
    vsnd.tx_error_status = 0;
    vsnd_retire_tx_locked();
    vsnd.submitted_bytes = 0;
    vsnd.completed_bytes = 0;
    vsnd.staged_bytes = 0;
    vsnd.staged_start_if_needed = 0;
    spin_unlock(&vsnd.lock);
}

static int vsnd_tx_period_nonblock_locked(int user, const void *buf,
                                          uint32 count, int start_if_needed)
{
    struct vsnd_queue *q = &vsnd.q[VSND_QUEUE_TX];
    uint32 period = count;
    uint32 slot = VSND_ASYNC_DEPTH;
    uint32 head;

    if (period > VSND_PERIOD_BYTES)
        period = VSND_PERIOD_BYTES;

    vsnd_poll_tx_completions();

    spin_lock(&vsnd.lock);
    if (vsnd.tx_error) {
        spin_unlock(&vsnd.lock);
        return -EPIPE;
    }
    uint64 busy_mask = vsnd.async_in_flight_mask;
    spin_unlock(&vsnd.lock);
    uint32 slot_limit = vsnd_async_slot_limit();
    for (uint32 i = 0; i < slot_limit; i++) {
        if ((busy_mask & (1ULL << i)) == 0) {
            slot = i;
            break;
        }
    }
    if (slot >= VSND_ASYNC_DEPTH)
        return -EAGAIN;
    head = VSND_ASYNC_BASE + slot * VSND_ASYNC_DESC_PER_SLOT;

    if (user) {
        if (either_copyin(vsnd.async_tx_buf[slot], 1, (uint64)buf, period) < 0)
            return -EFAULT;
    } else {
        memcpy(vsnd.async_tx_buf[slot], buf, period);
    }

    memset(&vsnd.async_xfer[slot], 0, sizeof(vsnd.async_xfer[slot]));
    memset(&vsnd.async_status[slot], 0, sizeof(vsnd.async_status[slot]));
    vsnd.async_xfer[slot].stream_id = vsnd.stream_id;

    spin_lock(&q->lock);
    q->desc[head].addr = vsnd_dma_addr(&vsnd.async_xfer[slot]);
    q->desc[head].len = sizeof(vsnd.async_xfer[slot]);
    q->desc[head].flags = VRING_DESC_F_NEXT;
    q->desc[head].next = head + 1;
    q->desc[head + 1].addr = vsnd_dma_addr(vsnd.async_tx_buf[slot]);
    q->desc[head + 1].len = period;
    q->desc[head + 1].flags = VRING_DESC_F_NEXT;
    q->desc[head + 1].next = head + 2;
    q->desc[head + 2].addr = vsnd_dma_addr(&vsnd.async_status[slot]);
    q->desc[head + 2].len = sizeof(vsnd.async_status[slot]);
    q->desc[head + 2].flags = VRING_DESC_F_WRITE;
    q->desc[head + 2].next = 0;
    q->avail->ring[q->avail->idx % q->size] = head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    spin_lock(&vsnd.lock);
    vsnd.submitted_bytes += period;
    vsnd.async_bytes[slot] = period;
    vsnd.async_in_flight_mask |= 1ULL << slot;
    spin_unlock(&vsnd.lock);
    spin_unlock(&q->lock);

    vsnd_notify(VSND_QUEUE_TX);
    if (start_if_needed && !vsnd.started) {
        int start_ret = vsnd_simple_cmd(VIRTIO_SND_R_PCM_START);
        if (start_ret < 0)
            return start_ret;
        vsnd.started = 1;
        spin_lock(&vsnd.lock);
        vsnd.play_started_ms = sched_timer_now_ms();
        spin_unlock(&vsnd.lock);
    }
    return (int)period;
}

/* vsnd.op_lock serializes the staged buffer and every submit/reset path. */
static int vsnd_stage_bytes_locked(int user, const void *buf, uint32 count,
                                   int start_if_needed)
{
    uint32 staged;
    uint32 chunk;

    spin_lock(&vsnd.lock);
    if (vsnd.tx_error) {
        spin_unlock(&vsnd.lock);
        return -EPIPE;
    }
    staged = vsnd.staged_bytes;
    spin_unlock(&vsnd.lock);

    if (staged >= VSND_PERIOD_BYTES)
        return -EAGAIN;
    chunk = VSND_PERIOD_BYTES - staged;
    if (chunk > count)
        chunk = count;

    if (user) {
        if (either_copyin(vsnd.tx_buf + staged, 1, (uint64)buf, chunk) < 0)
            return -EFAULT;
    } else {
        memcpy(vsnd.tx_buf + staged, buf, chunk);
    }

    spin_lock(&vsnd.lock);
    vsnd.staged_bytes += chunk;
    if (start_if_needed)
        vsnd.staged_start_if_needed = 1;
    spin_unlock(&vsnd.lock);
    return (int)chunk;
}

static int vsnd_submit_staged_nonblock_locked(void)
{
    uint32 staged;
    int start_if_needed;
    int ret;

    spin_lock(&vsnd.lock);
    staged = vsnd.staged_bytes;
    start_if_needed = vsnd.staged_start_if_needed;
    spin_unlock(&vsnd.lock);
    if (staged == 0)
        return 0;

    ret = vsnd_tx_period_nonblock_locked(0, vsnd.tx_buf, staged,
                                         start_if_needed);
    if (ret > 0) {
        spin_lock(&vsnd.lock);
        vsnd.staged_bytes = 0;
        vsnd.staged_start_if_needed = 0;
        spin_unlock(&vsnd.lock);
    }
    return ret;
}

static int vsnd_submit_staged_wait_locked(int timeout_ms)
{
    for (int waited = 0;; waited++) {
        int ret = vsnd_submit_staged_nonblock_locked();

        if (ret != -EAGAIN)
            return ret < 0 ? ret : 0;
        if (waited >= timeout_ms) {
            vsnd_tx_timeout_recover_locked(&vsnd.q[VSND_QUEUE_TX]);
            return -EIO;
        }
        vsnd_poll_tx_completions();
        sleep_ms(1);
    }
}

static int vsnd_write_mode(int user, const void *buf, size_t count,
                           int oss_format, int rate, int channels,
                           int nonblock, int start_if_needed)
{
    if (buf == NULL)
        return -EINVAL;
    if (!vsnd.initialized)
        return -ENODEV;

    mutex_lock(&vsnd.op_lock);
    int configure_waited_ms = 0;
    int ret;
    for (;;) {
        ret = vsnd_configure_locked(oss_format, rate, channels);
        if (ret != -EAGAIN || nonblock)
            break;

        /*
         * RELEASE completes every pending virtio-sound I/O message before
         * its control response, but the used-ring update can become visible
         * just after the response is consumed.  A blocking ALSA prequeue is
         * allowed to bridge that reset/reconfigure boundary; exporting the
         * transient EAGAIN makes PulseAudio abort in try_recover().
         */
        vsnd_poll_tx_completions();
        sleep_ms(1);
        if (++configure_waited_ms >= 1000) {
            ret = -EIO;
            break;
        }
    }
    if (ret < 0) {
        if (ret == -EAGAIN)
            printf("virtio_snd: write eagain source=configure nonblock=%d count=%lu\n",
                   nonblock, (uint64)count);
        mutex_unlock(&vsnd.op_lock);
        return ret;
    }

    size_t done = 0;
    int waited_ms = 0;
    while (done < count) {
        uint32 staged;

        spin_lock(&vsnd.lock);
        staged = vsnd.staged_bytes;
        spin_unlock(&vsnd.lock);
        if (staged < VSND_PERIOD_BYTES) {
            uint32 chunk = (uint32)(count - done);
            if (chunk > VSND_PERIOD_BYTES - staged)
                chunk = VSND_PERIOD_BYTES - staged;
            ret = vsnd_stage_bytes_locked(user, (const uint8 *)buf + done,
                                          chunk, start_if_needed);
            if (ret < 0) {
                mutex_unlock(&vsnd.op_lock);
                return done ? (int)done : ret;
            }
            done += (size_t)ret;
            waited_ms = 0;
            spin_lock(&vsnd.lock);
            staged = vsnd.staged_bytes;
            spin_unlock(&vsnd.lock);
            if (staged < VSND_PERIOD_BYTES)
                continue;
        }

        ret = vsnd_submit_staged_nonblock_locked();
        if (ret == -EAGAIN && !nonblock) {
            vsnd_poll_tx_completions();
            sleep_ms(1);
            if (++waited_ms >= 5000) {
                vsnd_tx_timeout_recover_locked(&vsnd.q[VSND_QUEUE_TX]);
                if (done != 0)
                    break;
                ret = -EIO;
            } else {
                continue;
            }
        }
        if (ret == -EAGAIN)
            break;
        if (ret < 0) {
            mutex_unlock(&vsnd.op_lock);
            return done ? (int)done : ret;
        }
        waited_ms = 0;
    }
    mutex_unlock(&vsnd.op_lock);
    return (int)done;
}

int virtio_snd_write(int user, const void *buf, size_t count, int oss_format,
                     int rate, int channels, int nonblock)
{
    return vsnd_write_mode(user, buf, count, oss_format, rate, channels,
                           nonblock, 1);
}

int virtio_snd_prequeue(int user, const void *buf, size_t count,
                        int oss_format, int rate, int channels)
{
    /* This is the serialized PREPARED -> RUNNING flush, never a user poll. */
    return vsnd_write_mode(user, buf, count, oss_format, rate, channels,
                           0, 0);
}

int virtio_snd_start(void)
{
    int ret = 0;

    if (!vsnd.initialized)
        return -ENODEV;
    mutex_lock(&vsnd.op_lock);
    if (!vsnd.configured) {
        ret = -EBADFD;
    } else {
        ret = vsnd_submit_staged_wait_locked(5000);
    }
    if (ret == 0 && !vsnd.started) {
        ret = vsnd_simple_cmd(VIRTIO_SND_R_PCM_START);
        if (ret == 0) {
            vsnd.started = 1;
            spin_lock(&vsnd.lock);
            vsnd.play_started_ms = sched_timer_now_ms();
            spin_unlock(&vsnd.lock);
        }
    }
    mutex_unlock(&vsnd.op_lock);
    return ret;
}

int virtio_snd_flush_partial(void)
{
    int ret;

    if (!vsnd.initialized)
        return -ENODEV;
    mutex_lock(&vsnd.op_lock);
    if (!vsnd.configured)
        ret = 0;
    else
        ret = vsnd_submit_staged_nonblock_locked();
    mutex_unlock(&vsnd.op_lock);
    return ret;
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
    vsnd_poll_tx_completions();
    spin_lock(&vsnd.lock);
    /*
     * STOP/RELEASE can leave QEMU-owned TX descriptors without a later used-ring
     * completion.  Retire them from the user-visible PCM stream, but do not
     * make their descriptor IDs reusable until QEMU returns used entries.
     */
    vsnd_roll_played_total_locked();
    vsnd_retire_tx_locked();
    vsnd.submitted_bytes = 0;
    vsnd.completed_bytes = 0;
    vsnd.staged_bytes = 0;
    vsnd.staged_start_if_needed = 0;
    vsnd.last_latency_bytes = 0;
    vsnd.last_latency_ms = 0;
    vsnd.play_started_ms = 0;
    vsnd.tx_error = 0;
    vsnd.tx_error_status = 0;
    vsnd.sync_bytes = 0;
    spin_unlock(&vsnd.lock);
    mutex_unlock(&vsnd.op_lock);
}

void virtio_snd_drain(void)
{
    uint64 start_ms = sched_timer_now_ms();
    uint64 last_progress_ms = start_ms;
    uint64 last_pending = ~0ULL;

    mutex_lock(&vsnd.op_lock);
    int flush_ret = vsnd_submit_staged_wait_locked(5000);
    mutex_unlock(&vsnd.op_lock);
    if (flush_ret < 0) {
        printf("virtio_snd: drain staged submit failed ret=%d\n", flush_ret);
        return;
    }

    while (virtio_snd_pending_bytes() != 0) {
        uint64 pending = virtio_snd_pending_bytes();
        uint64 now = sched_timer_now_ms();

        if (pending < last_pending) {
            last_pending = pending;
            last_progress_ms = now;
        }
        if (now - start_ms >= VSND_DRAIN_TIMEOUT_MS ||
            now - last_progress_ms >= VSND_DRAIN_TIMEOUT_MS) {
            printf("virtio_snd: drain timeout pending=%lu\n", pending);
            mutex_lock(&vsnd.op_lock);
            vsnd_tx_timeout_recover_locked(&vsnd.q[VSND_QUEUE_TX]);
            mutex_unlock(&vsnd.op_lock);
            return;
        }
        vsnd_poll_tx_completions();
        sleep_ms(1);
    }
}

uint64 virtio_snd_pending_bytes(void)
{
    if (!vsnd.initialized)
        return 0;
    vsnd_poll_tx_completions();
    spin_lock(&vsnd.lock);
    uint64 played = vsnd_paced_played_bytes_locked();
    uint64 pending = vsnd.submitted_bytes + vsnd.staged_bytes - played;
    spin_unlock(&vsnd.lock);
    return pending;
}

uint64 virtio_snd_free_bytes(void)
{
    if (!vsnd.initialized)
        return 0;
    vsnd_poll_tx_completions();
    spin_lock(&vsnd.lock);
    uint64 busy_mask = vsnd.async_in_flight_mask;
    uint32 staged = vsnd.staged_bytes;
    int tx_error = vsnd.tx_error;
    spin_unlock(&vsnd.lock);
    if (tx_error)
        return 0;
    uint32 free_slots = 0;
    uint32 slot_limit = vsnd_async_slot_limit();
    for (uint32 i = 0; i < slot_limit; i++) {
        if ((busy_mask & (1ULL << i)) == 0)
            free_slots++;
    }
    uint64 pending = virtio_snd_pending_bytes();
    if (pending >= VSND_BUFFER_BYTES)
        return 0;
    uint64 transport_free = (uint64)free_slots * VSND_PERIOD_BYTES;
    if (staged < VSND_PERIOD_BYTES)
        transport_free += VSND_PERIOD_BYTES - staged;
    uint64 logical_free = VSND_BUFFER_BYTES - pending;
    return transport_free < logical_free ? transport_free : logical_free;
}

uint64 virtio_snd_played_bytes(void)
{
    if (!vsnd.initialized)
        return 0;
    vsnd_poll_tx_completions();
    spin_lock(&vsnd.lock);
    uint64 played = vsnd_paced_played_bytes_locked();
    spin_unlock(&vsnd.lock);
    return played;
}

uint64 virtio_snd_pcm_hw_bytes(void)
{
    uint64 played;
    if (!vsnd.initialized)
        return 0;
    vsnd_poll_tx_completions();
    spin_lock(&vsnd.lock);
    played = vsnd_paced_played_bytes_locked();
    spin_unlock(&vsnd.lock);
    return played;
}

uint64 virtio_snd_total_played_bytes(void)
{
    if (!vsnd.initialized)
        return 0;
    vsnd_poll_tx_completions();
    spin_lock(&vsnd.lock);
    uint64 played = vsnd.total_played_bytes +
                    vsnd_paced_played_bytes_locked();
    spin_unlock(&vsnd.lock);
    return played;
}

uint64 virtio_snd_period_bytes(void)
{
    return VSND_PERIOD_BYTES;
}

uint64 virtio_snd_buffer_bytes(void)
{
    return VSND_BUFFER_BYTES;
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

int virtio_snd_supports_rate(int rate)
{
    int vrate;

    if (!vsnd.initialized)
        return 0;
    vrate = vsnd_rate_to_virtio(rate);
    return ((vsnd.rates >> vrate) & 1ULL) != 0;
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
        if (isr_status == 0)
            return;
    }
    vsnd_poll_tx_completions();
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
    for (uint32 i = 0; i < VSND_ASYNC_DEPTH; i++) {
        vsnd.async_tx_buf[i] = kalloc();
        if (vsnd.async_tx_buf[i] == NULL)
            panic("virtio_snd: kalloc async buffer");
    }
    memset(vsnd.events, 0, PGSIZE);
    memset(vsnd.tx_buf, 0, PGSIZE);
    for (uint32 i = 0; i < VSND_ASYNC_DEPTH; i++)
        memset(vsnd.async_tx_buf[i], 0, PGSIZE);
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
int virtio_snd_prequeue(int user, const void *buf, size_t count,
                        int oss_format, int rate, int channels)
{
    (void)user; (void)buf; (void)count; (void)oss_format;
    (void)rate; (void)channels;
    return -ENODEV;
}
int virtio_snd_start(void) { return -ENODEV; }
int virtio_snd_flush_partial(void) { return -ENODEV; }
void virtio_snd_reset(void) {}
void virtio_snd_drain(void) {}
uint64 virtio_snd_free_bytes(void) { return 0; }
uint64 virtio_snd_pending_bytes(void) { return 0; }
uint64 virtio_snd_played_bytes(void) { return 0; }
uint64 virtio_snd_pcm_hw_bytes(void) { return 0; }
uint64 virtio_snd_total_played_bytes(void) { return 0; }
uint64 virtio_snd_period_bytes(void) { return 0; }
uint64 virtio_snd_buffer_bytes(void) { return 0; }
int virtio_snd_supported_oss_formats(void) { return 0; }
int virtio_snd_supports_rate(int rate) { (void)rate; return 0; }
const char *virtio_snd_backend_name(void) { return "timer"; }

#endif
