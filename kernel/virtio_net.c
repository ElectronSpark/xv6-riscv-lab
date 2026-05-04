/*
 * virtio-net (modern PCI 1.0+) NIC driver for xv6-os.
 *
 * Coexists with the legacy e1000 driver: if QEMU exposes a virtio-net-pci
 * device it will be discovered during pci_init() and registered via the
 * netdev abstraction.  netdev_get_default() prefers virtio-net by name
 * when present so lwIP automatically uses the faster path.
 *
 * Single RX queue (idx 0) + single TX queue (idx 1).  No multiqueue, no
 * mergeable RX buffers.  Header is the modern 12-byte virtio_net_hdr.
 *
 * Layout per packet:
 *   TX: caller's mbuf has VIRTIO_NET_HDR_LEN headroom; we mbufpush() the
 *       header in place and submit a single descriptor (no chaining).
 *   RX: each pre-posted mbuf gets a single WRITE descriptor pointing to
 *       its backing page (room for header + 1500-byte frame + slack);
 *       on completion we strip the 12-byte header and pass the mbuf to
 *       net_rx().
 */
#include "compiler.h"
#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "printf.h"
#include "dev/net.h"
#include "dev/netdev.h"
#include "dev/pci.h"
#include "dev/virtio.h"
#include "trap.h"
#include "kstats.h"
#include "proc/workqueue.h"
#include "arch/vm.h"
#include <mm/vm.h>
#include <mm/pgtable.h>

/* Globals fed by e1000.c — extern here for kstats coherency.  Both NICs
 * share these counters; they aren't per-device. */
extern uint64 g_net_tx_packets;
extern uint64 g_net_tx_bytes;
extern uint64 g_net_rx_packets;
extern uint64 g_net_rx_bytes;

#define VNET_QUEUE_RX   0
#define VNET_QUEUE_TX   1
#define VNET_NQUEUES    2

/* Ring size; must be a power of two and <= queue_size negotiated with the
 * device.  128 matches the e1000 driver. */
#define VNET_RING_SIZE  128

/* Per-virtqueue state. */
struct vnet_vq {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;

    /* Software-shadowed indices. */
    uint16 last_used_idx;     /* next expected used->ring[] entry */
    uint16 next_avail_idx;    /* next slot we'll write into avail->ring[] */

    /* Free descriptor list (LIFO).  `free_head` is index of head, or
     * 0xFFFF if empty.  Linked via desc[i].next while on the freelist. */
    uint16 free_head;
    uint16 num_free;

    /* Track the mbuf parked in each descriptor slot so the IRQ can
     * recover it on completion. */
    struct mbuf *mbufs[VNET_RING_SIZE];

    /* Per-queue notification offset: queue_notify_off * mult bytes
     * past the start of the notify BAR. */
    uint16 queue_notify_off;

    spinlock_t lock;
};

/* Whole-device state. */
static struct vnet_state {
    struct vnet_vq vqs[VNET_NQUEUES];
    struct virtio_pci_state pci;
    volatile uint8 *dev_cfg;
    uint8 mac[6];
    uint8 irq_line;

    /* Negotiated feature flags (zero unless device offered them). */
    int feat_csum;        /* host handles partial checksum on TX */
    int feat_guest_csum;  /* device tells us when its checksum is good on RX */
    int feat_tso4;        /* host can do TSOv4 from us */
    int feat_tso6;
    int feat_mrg_rxbuf;
    int feat_mac;
    int feat_status;
    uint64 rx_bad_hdr;
    uint64 rx_bad_len;
    uint64 rx_need_csum;
    uint64 rx_gso;
    uint64 rx_merged;
} vnet;

static struct netdev vnet_ndev;

/* RX work is deferred to a kthread so we never call lwIP from IRQ context.
 * Matches the e1000 pattern. */
static struct workqueue *vnet_rx_wq;
static struct work_struct vnet_rx_work;
static volatile int vnet_rx_work_pending;

static int virtio_net_transmit(struct netdev *ndev, struct mbuf *m);
static void vnet_intr(int irq, void *data, device_t *dev);
static void vnet_rx_work_func(struct work_struct *work);
static void vnet_refill_rx(void);
static void vnet_reap_tx(void);

static struct netdev_ops virtio_net_ops = {
    .transmit = virtio_net_transmit,
};

/* ── helpers ───────────────────────────────────────────────────────── */

static inline void vnet_notify(int qi)
{
    volatile uint16 *na = (volatile uint16 *)
        ((uint8 *)vnet.pci.notify_base +
         vnet.vqs[qi].queue_notify_off * vnet.pci.notify_off_multiplier);
    *na = qi;
}

/* Pop a free descriptor index from the per-queue freelist.
 * Returns 0xFFFF if none available.  Caller holds vq->lock. */
static uint16 vnet_desc_alloc(struct vnet_vq *vq)
{
    if (vq->num_free == 0)
        return 0xFFFF;
    uint16 head = vq->free_head;
    vq->free_head = vq->desc[head].next;
    vq->num_free--;
    return head;
}

/* Push a descriptor back onto the freelist.  Caller holds vq->lock. */
static void vnet_desc_free(struct vnet_vq *vq, uint16 idx)
{
    vq->desc[idx].addr = 0;
    vq->desc[idx].len = 0;
    vq->desc[idx].flags = 0;
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;
}

/* ── MMIO window mapping (mirrors virtio_disk.c) ───────────────────── */

extern pagetable_t kernel_pagetable;

static uint64 vnet_map_mmio_window(uint64 bar, uint32 offset, uint32 length)
{
    if (bar & 0x1)
        panic("virtio_net: capability uses I/O BAR 0x%lx", bar);

    uint64 target = bar + offset;
    uint64 start = PGROUNDDOWN(target);
    uint64 end = PGROUNDUP(target + (length ? length : 1));
    uint64 size = end - start;
    uint64 map_base;
    vma_t *vma;

    vm_wlock(kernel_vm);
    map_base = vm_find_free_range(kernel_vm, size, 0);
    if (map_base == 0) {
        vm_wunlock(kernel_vm);
        panic("virtio_net: failed to allocate MMIO VA window");
    }
    vma = vma_alloc(kernel_vm, map_base, size,
                    PROT_READ | PROT_WRITE | VMA_FLAG_KERNEL);
    vm_wunlock(kernel_vm);
    if (vma == NULL)
        panic("virtio_net: failed to reserve MMIO VA window");

    for (uint64 page_off = 0; page_off < size; page_off += PGSIZE) {
        if (arch_vm_map(kernel_pagetable, map_base + page_off, PGSIZE,
                        start + page_off, PTE_R | PTE_W) != 0)
            panic("virtio_net: failed to map MMIO page");
    }
    arch_tlb_flush();
    return map_base + (target - start);
}

/* ── virtqueue setup ───────────────────────────────────────────────── */

static void vnet_vq_init_freelist(struct vnet_vq *vq)
{
    /* Build a linked freelist threaded through desc[i].next. */
    for (int i = 0; i < VNET_RING_SIZE - 1; i++)
        vq->desc[i].next = (uint16)(i + 1);
    vq->desc[VNET_RING_SIZE - 1].next = 0xFFFF;
    vq->free_head = 0;
    vq->num_free = VNET_RING_SIZE;
    vq->last_used_idx = 0;
    vq->next_avail_idx = 0;
}

static void vnet_vq_setup(int qi)
{
    struct vnet_vq *vq = &vnet.vqs[qi];
    volatile struct virtio_pci_common_cfg *cfg = vnet.pci.common_cfg;

    cfg->queue_select = qi;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint16 max = cfg->queue_size;
    if (max == 0)
        panic("virtio_net: queue %d not present", qi);
    if (max < VNET_RING_SIZE)
        panic("virtio_net: queue %d max %u < %d", qi, max, VNET_RING_SIZE);

    vq->desc  = kalloc();
    vq->avail = kalloc();
    vq->used  = kalloc();
    if (!vq->desc || !vq->avail || !vq->used)
        panic("virtio_net: vq %d kalloc", qi);
    memset(vq->desc, 0, PGSIZE);
    memset(vq->avail, 0, PGSIZE);
    memset(vq->used, 0, PGSIZE);

    cfg->queue_size = VNET_RING_SIZE;
    cfg->queue_desc   = (uint64)vq->desc;
    cfg->queue_driver = (uint64)vq->avail;
    cfg->queue_device = (uint64)vq->used;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->queue_enable = 1;

    vq->queue_notify_off = cfg->queue_notify_off;
    spin_init(&vq->lock, qi == VNET_QUEUE_RX ? "vnet_rx" : "vnet_tx");
    vnet_vq_init_freelist(vq);
    for (int i = 0; i < VNET_RING_SIZE; i++)
        vq->mbufs[i] = NULL;
}

/* ── RX path ───────────────────────────────────────────────────────── */

/* Post one descriptor pointing at the supplied mbuf into the RX ring.
 * Caller must hold rxq->lock.  Returns 0 on success, -1 if no descriptor
 * is free. */
static int vnet_post_rx_mbuf(struct mbuf *m)
{
    struct vnet_vq *vq = &vnet.vqs[VNET_QUEUE_RX];
    uint16 d = vnet_desc_alloc(vq);
    if (d == 0xFFFF)
        return -1;

    /* Whole 2KB backing page is writable by device. */
    vq->desc[d].addr = (uint64)m->buf;
    vq->desc[d].len = MBUF_SIZE;
    vq->desc[d].flags = VRING_DESC_F_WRITE;
    vq->desc[d].next = 0;
    vq->mbufs[d] = m;

    uint16 avail_slot = vq->next_avail_idx & (VNET_RING_SIZE - 1);
    vq->avail->ring[avail_slot] = d;
    vq->next_avail_idx++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vq->avail->idx = vq->next_avail_idx;
    return 0;
}

/* Fill the RX ring with fresh mbufs.  Called at init + after every
 * batch of RX completions. */
static void vnet_refill_rx(void)
{
    struct vnet_vq *vq = &vnet.vqs[VNET_QUEUE_RX];
    int posted = 0;

    spin_lock(&vq->lock);
    while (vq->num_free > 0) {
        struct mbuf *m = mbufalloc(0);
        if (!m)
            break;
        if (vnet_post_rx_mbuf(m) != 0) {
            mbuffree(m);
            break;
        }
        posted++;
    }
    spin_unlock(&vq->lock);

    if (posted > 0)
        vnet_notify(VNET_QUEUE_RX);
}

/* Drain the RX used ring.  Called from the deferred work item. */
static void vnet_drain_rx(void)
{
    struct vnet_vq *vq = &vnet.vqs[VNET_QUEUE_RX];

    for (;;) {
        spin_lock(&vq->lock);
        uint16 dev_idx = vq->used->idx;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (vq->last_used_idx == dev_idx) {
            spin_unlock(&vq->lock);
            break;
        }

        uint16 ue = vq->last_used_idx & (VNET_RING_SIZE - 1);
        uint32 d = vq->used->ring[ue].id;
        uint32 total_len = vq->used->ring[ue].len;
        struct mbuf *m = vq->mbufs[d];
        vq->mbufs[d] = NULL;
        vnet_desc_free(vq, (uint16)d);
        vq->last_used_idx++;
        spin_unlock(&vq->lock);

        if (m == NULL)
            continue;

        if (total_len < VIRTIO_NET_HDR_LEN) {
            vnet.rx_bad_len++;
            if (vnet.rx_bad_len <= 8 || (vnet.rx_bad_len % 256) == 0)
                printf("virtio-net: drop short rx len=%u total=%lu\n",
                       total_len, vnet.rx_bad_len);
            mbuffree(m);
            continue;
        }

        struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)m->buf;
        if (hdr->gso_type != VIRTIO_NET_HDR_GSO_NONE) {
            vnet.rx_gso++;
            if (vnet.rx_gso <= 8 || (vnet.rx_gso % 256) == 0)
                printf("virtio-net: drop rx gso type=%u hdr_len=%u "
                       "gso_size=%u total=%lu\n",
                       hdr->gso_type, hdr->hdr_len, hdr->gso_size,
                       vnet.rx_gso);
            mbuffree(m);
            continue;
        }
        if (hdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) {
            vnet.rx_need_csum++;
            if (vnet.rx_need_csum <= 8 || (vnet.rx_need_csum % 256) == 0)
                printf("virtio-net: drop rx needs-csum start=%u off=%u "
                       "total=%lu\n",
                       hdr->csum_start, hdr->csum_offset,
                       vnet.rx_need_csum);
            mbuffree(m);
            continue;
        }
        if (vnet.feat_mrg_rxbuf && hdr->num_buffers != 0 &&
            hdr->num_buffers != 1) {
            vnet.rx_merged++;
            if (vnet.rx_merged <= 8 || (vnet.rx_merged % 256) == 0)
                printf("virtio-net: drop merged rx buffers=%u len=%u "
                       "total=%lu\n",
                       hdr->num_buffers, total_len, vnet.rx_merged);
            mbuffree(m);
            continue;
        }
        if (total_len > MBUF_SIZE) {
            vnet.rx_bad_len++;
            if (vnet.rx_bad_len <= 8 || (vnet.rx_bad_len % 256) == 0)
                printf("virtio-net: drop oversized rx len=%u total=%lu\n",
                       total_len, vnet.rx_bad_len);
            mbuffree(m);
            continue;
        }

        m->head = m->buf + VIRTIO_NET_HDR_LEN;
        m->len  = total_len - VIRTIO_NET_HDR_LEN;

        g_net_rx_packets += 1;
        g_net_rx_bytes   += m->len;
        net_rx(m);  /* consumes m */
    }

    /* Repost replacement mbufs. */
    vnet_refill_rx();
}

static void vnet_rx_work_func(struct work_struct *work)
{
    (void)work;
    vnet_drain_rx();
    __atomic_store_n(&vnet_rx_work_pending, 0, __ATOMIC_RELEASE);

    /* Race: more packets may have arrived during drain.  Re-check. */
    struct vnet_vq *vq = &vnet.vqs[VNET_QUEUE_RX];
    if (vq->used->idx != vq->last_used_idx) {
        if (__atomic_exchange_n(&vnet_rx_work_pending, 1,
                                __ATOMIC_ACQ_REL) == 0) {
            if (!queue_work(vnet_rx_wq, &vnet_rx_work))
                __atomic_store_n(&vnet_rx_work_pending, 0,
                                 __ATOMIC_RELEASE);
        }
    }
}

static void vnet_schedule_rx(void)
{
    if (vnet_rx_wq == NULL)
        return;
    if (__atomic_exchange_n(&vnet_rx_work_pending, 1,
                            __ATOMIC_ACQ_REL) != 0)
        return;
    if (!queue_work(vnet_rx_wq, &vnet_rx_work))
        __atomic_store_n(&vnet_rx_work_pending, 0, __ATOMIC_RELEASE);
}

/* ── TX path ───────────────────────────────────────────────────────── */

/* Reap completed TX descriptors and free the carried mbufs.  Called from
 * both the IRQ and lazily from virtio_net_transmit (so we always have
 * descriptors available even if interrupts are coalesced). */
static void vnet_reap_tx(void)
{
    struct vnet_vq *vq = &vnet.vqs[VNET_QUEUE_TX];

    spin_lock(&vq->lock);
    uint16 dev_idx = vq->used->idx;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    while (vq->last_used_idx != dev_idx) {
        uint16 ue = vq->last_used_idx & (VNET_RING_SIZE - 1);
        uint32 d = vq->used->ring[ue].id;
        struct mbuf *m = vq->mbufs[d];
        vq->mbufs[d] = NULL;
        vnet_desc_free(vq, (uint16)d);
        vq->last_used_idx++;
        if (m)
            mbuffree(m);
    }
    spin_unlock(&vq->lock);
}

static int virtio_net_transmit(struct netdev *ndev, struct mbuf *m)
{
    (void)ndev;
    struct vnet_vq *vq = &vnet.vqs[VNET_QUEUE_TX];

    /* Need 12 bytes of headroom for the virtio_net_hdr. */
    if ((char *)m->head - (char *)m->buf < VIRTIO_NET_HDR_LEN) {
        /* Misconfigured caller — refuse rather than corrupt memory. */
        mbuffree(m);
        return -1;
    }

    /* Lazy reclaim before potentially sleeping on a free descriptor. */
    vnet_reap_tx();

    /* Prepend the (zeroed) header in the mbuf headroom. */
    struct virtio_net_hdr *hdr =
        (struct virtio_net_hdr *)mbufpush(m, VIRTIO_NET_HDR_LEN);
    memset(hdr, 0, sizeof(*hdr));
    /* num_buffers is reserved on TX; spec requires it be set to 0 by
     * the driver, which the memset handles. */

    int notify;
    spin_lock(&vq->lock);
    uint16 d = vnet_desc_alloc(vq);
    if (d == 0xFFFF) {
        /* Ring full — best-effort: drop.  lwIP will retry on next ack. */
        spin_unlock(&vq->lock);
        mbuffree(m);
        return -1;
    }
    vq->desc[d].addr = (uint64)m->head;
    vq->desc[d].len = m->len;
    vq->desc[d].flags = 0;       /* device READ */
    vq->desc[d].next = 0;
    vq->mbufs[d] = m;

    uint16 avail_slot = vq->next_avail_idx & (VNET_RING_SIZE - 1);
    vq->avail->ring[avail_slot] = d;
    vq->next_avail_idx++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vq->avail->idx = vq->next_avail_idx;
    notify = 1;
    spin_unlock(&vq->lock);

    if (notify)
        vnet_notify(VNET_QUEUE_TX);

    g_net_tx_packets += 1;
    g_net_tx_bytes   += (m->len - VIRTIO_NET_HDR_LEN);
    return 0;
}

/* ── IRQ ───────────────────────────────────────────────────────────── */

static void vnet_intr(int irq, void *data, device_t *dev)
{
    (void)irq; (void)data; (void)dev;
    /* Read & clear ISR. */
    uint8 isr = *vnet.pci.isr;
    if (isr == 0)
        return;
    /* Bit 0: vring activity (RX or TX or both). */
    vnet_reap_tx();
    vnet_schedule_rx();
}

/* ── PCI init ──────────────────────────────────────────────────────── */

static void vnet_init_pci(struct virtio_pci_discovery *vd)
{
    vnet.pci.use_pci = 1;
    vnet.irq_line = vd->irq_line;

    uint8 ccap = vd->common_cfg_cap;
    uint8 ncap = vd->notify_cfg_cap;
    uint8 icap = vd->isr_cfg_cap;
    uint8 dcap = vd->device_cfg_cap;
    if (!ccap || !ncap || !icap)
        panic("virtio_net: missing PCI capabilities");

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

    uint64 cc_base = (uint64)(vd->bar[cc_bar] & ~0xFU);
    uint64 n_base  = (uint64)(vd->bar[n_bar]  & ~0xFU);
    uint64 i_base  = (uint64)(vd->bar[i_bar]  & ~0xFU);
    if ((vd->bar[cc_bar] & 0x6) == 0x4 && cc_bar < 5)
        cc_base |= ((uint64)vd->bar[cc_bar + 1]) << 32;
    if ((vd->bar[n_bar] & 0x6) == 0x4 && n_bar < 5)
        n_base |= ((uint64)vd->bar[n_bar + 1]) << 32;
    if ((vd->bar[i_bar] & 0x6) == 0x4 && i_bar < 5)
        i_base |= ((uint64)vd->bar[i_bar + 1]) << 32;

    uint64 cc_va = vnet_map_mmio_window(cc_base, cc_off, cc_len);
    uint64 n_va  = vnet_map_mmio_window(n_base,  n_off,  n_len);
    uint64 i_va  = vnet_map_mmio_window(i_base,  i_off,  i_len);

    volatile uint8 *dev_cfg = NULL;
    if (dcap) {
        uint8 d_bar = pci_config_read8(vd->bus, vd->dev, vd->func, dcap + 4);
        uint32 d_off = pci_config_read32(vd->bus, vd->dev, vd->func, dcap + 8);
        uint32 d_len = pci_config_read32(vd->bus, vd->dev, vd->func, dcap + 12);
        uint64 d_base = (uint64)(vd->bar[d_bar] & ~0xFU);
        if ((vd->bar[d_bar] & 0x6) == 0x4 && d_bar < 5)
            d_base |= ((uint64)vd->bar[d_bar + 1]) << 32;
        dev_cfg = (volatile uint8 *)vnet_map_mmio_window(d_base, d_off, d_len);
    }

    vnet.pci.common_cfg = (volatile struct virtio_pci_common_cfg *)cc_va;
    vnet.pci.notify_base = (volatile uint16 *)n_va;
    vnet.pci.notify_off_multiplier = n_mult;
    vnet.pci.isr = (volatile uint8 *)i_va;
    vnet.dev_cfg = dev_cfg;

    volatile struct virtio_pci_common_cfg *cfg = vnet.pci.common_cfg;

    /* Spec §3.1.1 init sequence. */
    cfg->device_status = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (cfg->device_status != 0)
        ;

    uint8 status = 0;
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE; cfg->device_status = status;
    status |= VIRTIO_CONFIG_S_DRIVER;      cfg->device_status = status;

    /* Read offered features (words 0 and 1). */
    cfg->device_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32 feat0 = cfg->device_feature;
    cfg->device_feature_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32 feat1 = cfg->device_feature;

    vnet.feat_mac        = !!(feat0 & (1u << VIRTIO_NET_F_MAC));
    vnet.feat_status     = !!(feat0 & (1u << VIRTIO_NET_F_STATUS));
    vnet.feat_csum       = !!(feat0 & (1u << VIRTIO_NET_F_CSUM));
    vnet.feat_guest_csum = !!(feat0 & (1u << VIRTIO_NET_F_GUEST_CSUM));
    vnet.feat_tso4       = !!(feat0 & (1u << VIRTIO_NET_F_HOST_TSO4));
    vnet.feat_tso6       = !!(feat0 & (1u << VIRTIO_NET_F_HOST_TSO6));
    vnet.feat_mrg_rxbuf  = !!(feat0 & (1u << VIRTIO_NET_F_MRG_RXBUF));
    int feat_version_1   = !!(feat1 & (1u << (VIRTIO_F_VERSION_1 - 32)));

    /* MVP feature negotiation: only MAC + STATUS + VERSION_1.
     *
     * Other bits the device may offer (and why we don't ack them yet):
     *
     *   F_CSUM (TX checksum offload)
     *      Requires LWIP_CHECKSUM_CTRL_PER_NETIF + driver-side parsing
     *      of the L4 header to populate csum_start/csum_offset, and
     *      careful coordination with lwIP so it writes the pseudo-header
     *      partial sum (vs leaving the field zero).
     *
     *   GUEST_CSUM
     *      NOT free: acking this commits us to completing partial
     *      checksums on RX (host may flag frames NEEDS_CSUM and hand us
     *      pseudo-header-only sums).  lwIP will reject them as bad
     *      checksums otherwise — observed on TAP.
     *
     *   GUEST_TSO4/6 + MRG_RXBUF
     *      Requires multi-mbuf RX scatter; a coalesced segment can be up
     *      to 64 KiB but our mbuf backing store is one 2 KiB page. */
    uint32 ack0 = 0;
    if (vnet.feat_mac)    ack0 |= (1u << VIRTIO_NET_F_MAC);
    if (vnet.feat_status) ack0 |= (1u << VIRTIO_NET_F_STATUS);
    if (vnet.feat_mrg_rxbuf) ack0 |= (1u << VIRTIO_NET_F_MRG_RXBUF);
    uint32 ack1 = 0;
    if (feat_version_1)   ack1 |= (1u << (VIRTIO_F_VERSION_1 - 32));

    cfg->driver_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->driver_feature = ack0;
    cfg->driver_feature_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->driver_feature = ack1;

    status |= VIRTIO_CONFIG_S_FEATURES_OK; cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(cfg->device_status & VIRTIO_CONFIG_S_FEATURES_OK))
        panic("virtio_net: FEATURES_OK rejected");

    /* MAC address: virtio_net_config.mac is at device_cfg offset 0. */
    if (vnet.feat_mac && dev_cfg) {
        for (int i = 0; i < 6; i++)
            vnet.mac[i] = dev_cfg[i];
    } else {
        /* Sensible default; QEMU's default MAC is 52:54:00:12:34:56. */
        const uint8 def[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};
        for (int i = 0; i < 6; i++)
            vnet.mac[i] = def[i];
    }

    /* Set up RX (queue 0) and TX (queue 1). */
    vnet_vq_setup(VNET_QUEUE_RX);
    vnet_vq_setup(VNET_QUEUE_TX);

    /* IRQ. */
    static struct irq_desc vnet_irq_desc;
    vnet_irq_desc.handler = vnet_intr;
    vnet_irq_desc.data = NULL;
    vnet_irq_desc.dev = NULL;
    int rc = register_irq_handler(PLIC_IRQ(vd->irq_line), &vnet_irq_desc);
    if (rc != 0)
        panic("virtio_net: register_irq_handler -> %d", rc);
    extern void plic_enable_irq_level(int irq);
    plic_enable_irq_level(vd->irq_line);

    /* RX work queue (deferred RX into lwIP). */
    init_work_struct(&vnet_rx_work, vnet_rx_work_func, 0);
    vnet_rx_wq = workqueue_create("virtio_net_rx", 2);
    if (!vnet_rx_wq)
        panic("virtio_net: rx workqueue");

    /* DRIVER_OK — device may now process queues. */
    status |= VIRTIO_CONFIG_S_DRIVER_OK; cfg->device_status = status;

    /* Initial RX fill. */
    vnet_refill_rx();

    /* Register with the netdev abstraction. */
    strncpy(vnet_ndev.name, "virtio-net", NETDEV_NAME_MAX);
    memmove(vnet_ndev.mac, vnet.mac, 6);
    vnet_ndev.mtu = 1500;
    vnet_ndev.link_up = 1;
    vnet_ndev.speed = 1000;
    vnet_ndev.full_duplex = 1;
    vnet_ndev.ops = &virtio_net_ops;
    vnet_ndev.priv = &vnet;
    netdev_register(&vnet_ndev);

    printf("virtio-net: ready, MAC %x:%x:%x:%x:%x:%x irq=%d feats: csum=%d gcsum=%d tso4=%d tso6=%d mrg=%d ver1=%d\n",
           vnet.mac[0], vnet.mac[1], vnet.mac[2],
           vnet.mac[3], vnet.mac[4], vnet.mac[5], vd->irq_line,
           vnet.feat_csum, vnet.feat_guest_csum,
           vnet.feat_tso4, vnet.feat_tso6, vnet.feat_mrg_rxbuf,
           feat_version_1);
}

void virtio_net_init(void)
{
    struct virtio_pci_discovery *vd = pci_get_virtio_net(0);
    if (!vd || !vd->found)
        return;
    vnet_init_pci(vd);
}
