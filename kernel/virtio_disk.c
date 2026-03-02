//
// driver for qemu's virtio disk device.
// Supports both MMIO transport (RISC-V) and PCI transport (x86).
//
// MMIO: qemu ... -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
// PCI:  qemu ... -device virtio-blk-pci,drive=x0
//

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "lock/completion.h"
#include "proc/tq.h"
#include "vfs/xv6fs/ondisk.h" // for BSIZE
#include "dev/buf.h"
#include "dev/bio.h"
#include "dev/virtio.h"
#include "dev/pci.h"
#include "dev/blkdev.h"
#include "dev/gendisk.h"
#include <mm/page.h>
#include "errno.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "trap.h"
#include "freelist.h"
#include "dev/fdt.h"

// the address of virtio mmio register r for disk n (MMIO transport only).
#define R(n, r) ((volatile uint32 *)(__virtio_mmio_base[n] + (r)))

// These are initialized from platform info at runtime
uint64 __virtio_mmio_base[N_VIRTIO] = {0x10001000, 0x10002000, 0x10003000};
uint64 __virtio_irqno[N_VIRTIO] = {1, 2, 3};

static void virtio_disk_rw(int diskno, struct bio *bio, uint64 sector,
                           void *buf, size_t size, int write);
static void virtio_disk_intr(int irq, void *data, device_t *dev);

/// Minor number stride per disk: disk0=1, disk1=17, disk2=33, etc.
/// Partitions occupy minors base+1 .. base+15.
#define GENDISK_MINOR_STRIDE 16

STATIC struct disk {
    // a set (not a ring) of DMA descriptors, with which the
    // driver tells the device where to read and write individual
    // disk operations. there are NUM descriptors.
    // most commands consist of a "chain" (a linked list) of a couple of
    // these descriptors.
    struct virtq_desc *desc;

    // a ring in which the driver writes descriptor numbers
    // that the driver would like the device to process.  it only
    // includes the head descriptor of each chain. the ring has
    // NUM elements.
    struct virtq_avail *avail;

    // a ring in which the device writes descriptor numbers that
    // the device has finished processing (just the head of each chain).
    // there are NUM used ring entries.
    struct virtq_used *used;

    // our own book-keeping.
    char free[NUM];                // is a descriptor free?
    uint16 used_idx;               // we've looked this far in used[2..NUM].
    uint16 free_list[NUM];         // The index of the free descriptors
    struct freelist desc_freelist; // Freelist manager for descriptors

    // track info about in-flight operations,
    // for use when completion interrupt arrives.
    // indexed by first descriptor index of chain.
    struct {
        struct bio *bio;
        bool done;
        char status;
    } info[NUM];

    // disk command headers.
    // one-for-one with descriptors, for convenience.
    struct virtio_blk_req ops[NUM];

    spinlock_t vdisk_lock;

    /**
     * Wait queue for processes waiting for free descriptors.
     * Used instead of global sleep_on_chan() to avoid contention
     * on the global sleep_lock. Processes waiting in alloc3_desc()
     * sleep on this per-disk queue and are woken when free_desc()
     * returns descriptors to the freelist.
     */
    tq_t desc_wait_queue;

    // PCI transport state (valid when pci_state.use_pci == 1)
    struct virtio_pci_state pci_state;

} disks[N_VIRTIO_DISK];

static int __virtio_disk_open(blkdev_t *blkdev) { return 0; }

static int __virtio_disk_release(blkdev_t *blkdev) { return 0; }

static int __virtio_disk_submit_bio(blkdev_t *blkdev, struct bio *bio) {
    struct bio_vec bvec;
    struct bio_iter iter;
    int diskno = (blkdev->dev.minor - 1) / GENDISK_MINOR_STRIDE;

    bio_start_io_acct(bio);
    bio_for_each_segment(&bvec, bio, &iter) {
        uint64 sector = iter.blkno;
        page_t *page = bvec.bv_page;
        assert(page != NULL, "virtio_disk_submit_bio: page is NULL");
        void *pa = (void *)__page_to_pa(page);
        assert(pa != NULL,
               "virtio_disk_submit_bio: page has no physical address");
        virtio_disk_rw(diskno, bio, sector, pa + bvec.offset, bvec.len,
                       bio_dir_write(bio));
    }

    // I/O has been submitted to the device.  Completion (bio_complete)
    // is signalled by virtio_disk_intr when the device finishes.
    // Callers wait via bio_await().
    return 0;
}

static blkdev_ops_t __virtio_disk_ops = {.open = __virtio_disk_open,
                                         .release = __virtio_disk_release,
                                         .submit_bio =
                                             __virtio_disk_submit_bio};

static blkdev_t virtio_disk_devs[N_VIRTIO_DISK] = {
    {
        .dev =
            {
                .major = 2,
                .minor = 1,   /* disk0: base minor 1, parts 2..16 */
                .devname = "disk0",
                .devmode = S_IFBLK | 0600,
            },
        .readable = 1,
        .writable = 1,
        .block_shift = 0, // 2^0 * 512 = 512 bytes per block
    },
    {
        .dev =
            {
                .major = 2,
                .minor = 17,  /* disk1: base minor 17, parts 18..32 */
                .devname = "disk1",
                .devmode = S_IFBLK | 0600,
            },
        .readable = 1,
        .writable = 1,
        .block_shift = 0, // 2^0 * 512 = 512 bytes per block
    },
};

#if !defined(__x86_64__) && !defined(__i386__)
static void __virtio_blkdev_init(int diskno) {
    virtio_disk_devs[diskno].ops = __virtio_disk_ops;
    int errno = blkdev_register(&virtio_disk_devs[diskno]);
    assert(errno == 0, "virtio_blkdev_init: blkdev_register failed: %d", errno);
    struct irq_desc virtio_irq_desc = {
        .handler = virtio_disk_intr,
        .data = (void *)(uint64)diskno,
        .dev = &virtio_disk_devs[diskno].dev,
    };
    errno =
        register_irq_handler(PLIC_IRQ(VIRTIO0_IRQ + diskno), &virtio_irq_desc);
    assert(errno == 0, "virtio_blkdev_init: register_irq_handler failed: %d",
           errno);
    /* Probe for GPT/MBR partitions — must be after IRQ registration
     * so that BIO completions can be delivered. */
    gendisk_probe(&virtio_disk_devs[diskno]);
}

static void __virtio_disk_init_one(int diskno) {
    struct disk *disk = &disks[diskno];
    uint32 status = 0;

    spin_init(&disk->vdisk_lock, "virtio_disk");

    if (*R(diskno, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
        *R(diskno, VIRTIO_MMIO_VERSION) != 2 ||
        *R(diskno, VIRTIO_MMIO_DEVICE_ID) != 2 ||
        *R(diskno, VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
        printf("virtio_disk: slot %d is not a block device, skipping\n",
               diskno);
        return;
    }

    // reset device
    *R(diskno, VIRTIO_MMIO_STATUS) = status;

    // set ACKNOWLEDGE status bit
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *R(diskno, VIRTIO_MMIO_STATUS) = status;

    // set DRIVER status bit
    status |= VIRTIO_CONFIG_S_DRIVER;
    *R(diskno, VIRTIO_MMIO_STATUS) = status;

    // negotiate features
    uint64 features = *R(diskno, VIRTIO_MMIO_DEVICE_FEATURES);
    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    *R(diskno, VIRTIO_MMIO_DRIVER_FEATURES) = features;

    // tell device that feature negotiation is complete.
    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(diskno, VIRTIO_MMIO_STATUS) = status;

    // re-read status to ensure FEATURES_OK is set.
    status = *R(diskno, VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
        panic("virtio disk %d FEATURES_OK unset", diskno);

    // initialize queue 0.
    *R(diskno, VIRTIO_MMIO_QUEUE_SEL) = 0;

    // ensure queue 0 is not in use.
    if (*R(diskno, VIRTIO_MMIO_QUEUE_READY))
        panic("virtio disk %d should not be ready", diskno);

    // check maximum queue size.
    uint32 max = *R(diskno, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0)
        panic("virtio disk %d has no queue 0", diskno);
    if (max < NUM)
        panic("virtio disk %d max queue too short", diskno);

    // allocate and zero queue memory.
    disk->desc = kalloc();
    disk->avail = kalloc();
    disk->used = kalloc();
    if (!disk->desc || !disk->avail || !disk->used)
        panic("virtio disk %d kalloc", diskno);
    memset(disk->desc, 0, PGSIZE);
    memset(disk->avail, 0, PGSIZE);
    memset(disk->used, 0, PGSIZE);

    // set queue size.
    *R(diskno, VIRTIO_MMIO_QUEUE_NUM) = NUM;

    // write physical addresses.
    *R(diskno, VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk->desc;
    *R(diskno, VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk->desc >> 32;
    *R(diskno, VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk->avail;
    *R(diskno, VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk->avail >> 32;
    *R(diskno, VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk->used;
    *R(diskno, VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk->used >> 32;

    // queue is ready.
    *R(diskno, VIRTIO_MMIO_QUEUE_READY) = 0x1;

    // all NUM descriptors start out unused.
    freelist_init(&disk->desc_freelist, disk->free, disk->free_list, NUM);
    tq_init(&disk->desc_wait_queue, "virtio_desc_wait", &disk->vdisk_lock);

    // tell device we're completely ready.
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(diskno, VIRTIO_MMIO_STATUS) = status;

    __virtio_blkdev_init(diskno);
    // plic.c and trap.c arrange for interrupts from VIRTIO IRQs.
}
#endif /* !__x86_64__ && !__i386__ */

#if defined(__x86_64__) || defined(__i386__)
//
// PCI transport initialization for x86.
// Uses virtio modern PCI capabilities to find BAR regions.
//
static void __virtio_blkdev_init_pci(int diskno, uint8 irq_line) {
    virtio_disk_devs[diskno].ops = __virtio_disk_ops;
    int errno = blkdev_register(&virtio_disk_devs[diskno]);
    assert(errno == 0, "virtio_blkdev_init_pci: blkdev_register failed: %d",
           errno);
    struct irq_desc virtio_irq_desc = {
        .handler = virtio_disk_intr,
        .data = (void *)(uint64)diskno,
        .dev = &virtio_disk_devs[diskno].dev,
    };
    // On x86, PCI device uses IOAPIC IRQ line from PIIX3 routing
    errno = register_irq_handler(PLIC_IRQ(irq_line), &virtio_irq_desc);
    if (errno != 0) {
        printf("virtio_blkdev_init_pci: WARNING: IRQ %d registration failed "
               "(%d), disk %d interrupts unavailable\n",
               irq_line, errno, diskno);
        return;
    }
    // PCI interrupts are level-triggered, active-low
    extern void plic_enable_irq_level(int irq);
    plic_enable_irq_level(irq_line);
    /* Probe for GPT/MBR partitions — must be after IRQ registration
     * so that BIO completions can be delivered. */
    gendisk_probe(&virtio_disk_devs[diskno]);
}

static void __virtio_disk_init_one_pci(int diskno) {
    struct virtio_pci_discovery *vd = pci_get_virtio_blk(diskno);
    if (!vd || !vd->found) {
        printf("virtio_disk_pci: no PCI device for disk %d\n", diskno);
        return;
    }

    struct disk *disk = &disks[diskno];
    spin_init(&disk->vdisk_lock, "virtio_disk_pci");
    disk->pci_state.use_pci = 1;

    // Read common config capability: bar, offset, length
    uint8 ccap = vd->common_cfg_cap;
    uint8 ncap = vd->notify_cfg_cap;
    uint8 icap = vd->isr_cfg_cap;

    if (!ccap || !ncap || !icap)
        panic("virtio_disk_pci: missing PCI capabilities");

    // Read cap fields via PCI config space
    uint8 cc_bar = pci_config_read8(vd->bus, vd->dev, vd->func, ccap + 4);
    uint32 cc_off = pci_config_read32(vd->bus, vd->dev, vd->func, ccap + 8);

    uint8 n_bar = pci_config_read8(vd->bus, vd->dev, vd->func, ncap + 4);
    uint32 n_off = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 8);
    uint32 n_mult = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 16);

    uint8 i_bar = pci_config_read8(vd->bus, vd->dev, vd->func, icap + 4);
    uint32 i_off = pci_config_read32(vd->bus, vd->dev, vd->func, icap + 8);

    // Get BAR base addresses (mask off type bits)
    uint64 cc_base = (uint64)(vd->bar[cc_bar] & ~0xFU);
    uint64 n_base = (uint64)(vd->bar[n_bar] & ~0xFU);
    uint64 i_base = (uint64)(vd->bar[i_bar] & ~0xFU);

    // If BAR is 64-bit (bit 2:1 = 10b), combine with next BAR
    if ((vd->bar[cc_bar] & 0x6) == 0x4 && cc_bar < 5)
        cc_base |= ((uint64)vd->bar[cc_bar + 1]) << 32;
    if ((vd->bar[n_bar] & 0x6) == 0x4 && n_bar < 5)
        n_base |= ((uint64)vd->bar[n_bar + 1]) << 32;
    if ((vd->bar[i_bar] & 0x6) == 0x4 && i_bar < 5)
        i_base |= ((uint64)vd->bar[i_bar + 1]) << 32;

    printf("virtio_disk_pci: disk %d common@BAR%d+0x%x notify@BAR%d+0x%x(mult=%d) isr@BAR%d+0x%x\n",
           diskno, cc_bar, cc_off, n_bar, n_off, n_mult, i_bar, i_off);
    printf("virtio_disk_pci: BAR bases: common=0x%lx notify=0x%lx isr=0x%lx\n",
           cc_base, n_base, i_base);

    // Map BAR regions (identity mapped on x86 kernel)
    volatile struct virtio_pci_common_cfg *cfg =
        (volatile struct virtio_pci_common_cfg *)(cc_base + cc_off);
    volatile uint16 *notify = (volatile uint16 *)(n_base + n_off);
    volatile uint8 *isr = (volatile uint8 *)(i_base + i_off);

    disk->pci_state.common_cfg = cfg;
    disk->pci_state.notify_base = notify;
    disk->pci_state.notify_off_multiplier = n_mult;
    disk->pci_state.isr = isr;

    // --- Virtio initialization sequence (modern PCI) ---

    // Reset device
    cfg->device_status = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // Wait for reset to complete
    while (cfg->device_status != 0)
        ;

    uint8 status = 0;

    // Set ACKNOWLEDGE
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    cfg->device_status = status;

    // Set DRIVER
    status |= VIRTIO_CONFIG_S_DRIVER;
    cfg->device_status = status;

    // Negotiate features (read features word 0)
    cfg->device_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32 features = cfg->device_feature;
    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    cfg->driver_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->driver_feature = features;

    // FEATURES_OK
    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    status = cfg->device_status;
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
        panic("virtio disk pci %d FEATURES_OK unset", diskno);

    // Initialize queue 0
    cfg->queue_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint16 max = cfg->queue_size;
    if (max == 0)
        panic("virtio disk pci %d has no queue 0", diskno);
    if (max < NUM) {
        printf("virtio_disk_pci: max queue size %d, using that\n", max);
    }

    // Allocate and zero queue memory
    disk->desc = kalloc();
    disk->avail = kalloc();
    disk->used = kalloc();
    if (!disk->desc || !disk->avail || !disk->used)
        panic("virtio disk pci %d kalloc", diskno);
    memset(disk->desc, 0, PGSIZE);
    memset(disk->avail, 0, PGSIZE);
    memset(disk->used, 0, PGSIZE);

    // Set queue size
    cfg->queue_size = NUM;

    // Write physical addresses of virtqueue components
    cfg->queue_desc = (uint64)disk->desc;
    cfg->queue_driver = (uint64)disk->avail;
    cfg->queue_device = (uint64)disk->used;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // Enable queue
    cfg->queue_enable = 1;

    // Save queue_notify_off for notification
    uint16 q_notify_off = cfg->queue_notify_off;

    // Initialize freelist and wait queue
    freelist_init(&disk->desc_freelist, disk->free, disk->free_list, NUM);
    tq_init(&disk->desc_wait_queue, "virtio_desc_wait_pci",
            &disk->vdisk_lock);

    // DRIVER_OK
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    cfg->device_status = status;

    printf("virtio_disk_pci: disk %d initialized, queue_notify_off=%d irq=%d\n",
           diskno, q_notify_off, vd->irq_line);

    __virtio_blkdev_init_pci(diskno, vd->irq_line);
}
#endif /* __x86_64__ */

void virtio_disk_init(void) {
    if (!platform.has_virtio || platform.virtio_count == 0)
        return;

    int num_disks = platform.virtio_count;
    if (num_disks > N_VIRTIO_DISK)
        num_disks = N_VIRTIO_DISK;

#if defined(__x86_64__) || defined(__i386__)
    // On x86, use PCI transport
    for (int i = 0; i < num_disks; i++) {
        __virtio_disk_init_one_pci(i);
    }
#else
    // On RISC-V, use MMIO transport
    for (int i = 0; i < num_disks; i++) {
        __virtio_disk_init_one(i);
    }
#endif
}

// find a free descriptor, mark it non-free, return its index.
STATIC int alloc_desc(struct disk *disk) {
    return freelist_alloc(&disk->desc_freelist);
}

// mark a descriptor as free.
STATIC void free_desc(struct disk *disk, int i) {
    if (freelist_free(&disk->desc_freelist, i) != 0)
        panic("free_desc: invalid free");

    disk->desc[i].addr = 0;
    disk->desc[i].len = 0;
    disk->desc[i].flags = 0;
    disk->desc[i].next = 0;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // Wake waiters if enough descriptors are free
    // Note: We're already holding vdisk_lock, so wake directly
    if (freelist_available(&disk->desc_freelist) >= 3) {
        tq_wakeup_all(&disk->desc_wait_queue, 0, 0);
    }
}

// free a chain of descriptors.
STATIC void free_chain(struct disk *disk, int i) {
    while (1) {
        int flag = disk->desc[i].flags;
        int nxt = disk->desc[i].next;
        free_desc(disk, i);
        if (flag & VRING_DESC_F_NEXT)
            i = nxt;
        else
            break;
    }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
STATIC int alloc3_desc(struct disk *disk, int *idx) {
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    for (int i = 0; i < 3; i++) {
        idx[i] = alloc_desc(disk);
        if (idx[i] < 0) {
            for (int j = 0; j < i; j++)
                free_desc(disk, idx[j]);
            return -1;
        }
    }
    return 0;
}

static void virtio_disk_rw(int diskno, struct bio *bio, uint64 sector,
                           void *buf, size_t size, int write) {
    struct disk *disk = &disks[diskno];
    assert(size > 0 && size <= BIO_MAX_SIZE,
           "virtio_disk_rw: size must be >0 and <=BIO_MAX_SIZE");
    assert(buf != NULL, "virtio_disk_rw: buf is NULL");

    spin_lock(&disk->vdisk_lock);

    // the spec's Section 5.2 says that legacy block operations use
    // three descriptors: one for type/reserved/sector, one for the
    // data, one for a 1-byte status result.

    // allocate the three descriptors.
    int idx[3];
    while (1) {
        if (alloc3_desc(disk, idx) == 0) {
            break;
        }
        // No free descriptors, wait on per-disk queue
        __thread_state_set(current, THREAD_UNINTERRUPTIBLE);
        tq_wait(&disk->desc_wait_queue, &disk->vdisk_lock, NULL);
    }

    // format the three descriptors.
    // qemu's virtio-blk.c reads them.

    struct virtio_blk_req *buf0 = &disk->ops[idx[0]];

    if (write)
        buf0->type = VIRTIO_BLK_T_OUT; // write the disk
    else
        buf0->type = VIRTIO_BLK_T_IN; // read the disk
    buf0->reserved = 0;
    buf0->sector = sector;

    disk->desc[idx[0]].addr = (uint64)buf0;
    disk->desc[idx[0]].len = sizeof(struct virtio_blk_req);
    disk->desc[idx[0]].flags = VRING_DESC_F_NEXT;
    disk->desc[idx[0]].next = idx[1];

    disk->desc[idx[1]].addr = (uint64)buf;
    disk->desc[idx[1]].len = size;
    if (write)
        disk->desc[idx[1]].flags = 0; // device reads b->data
    else
        disk->desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
    disk->desc[idx[1]].flags |= VRING_DESC_F_NEXT;
    disk->desc[idx[1]].next = idx[2];

    disk->info[idx[0]].status = 0xff; // device writes 0 on success
    disk->desc[idx[2]].addr = (uint64)&disk->info[idx[0]].status;
    disk->desc[idx[2]].len = 1;
    disk->desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
    disk->desc[idx[2]].next = 0;

    // record struct buf for virtio_disk_intr().
    bio->private_data = NULL;
    disk->info[idx[0]].bio = bio;

    // tell the device the first index in our chain of descriptors.
    disk->avail->ring[disk->avail->idx % NUM] = idx[0];

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // tell the device another avail ring entry is available.
    disk->avail->idx += 1; // not % NUM ...

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    assert(!intr_get(), "virtio_disk_rw: interrupts enabled");

    // Notify the device: PCI vs MMIO transport
    if (disk->pci_state.use_pci) {
        // PCI: write queue index to notify region
        // offset = queue_notify_off * notify_off_multiplier (in bytes)
        // For queue 0, queue_notify_off is typically 0
        volatile uint16 *notify_addr = disk->pci_state.notify_base;
        *notify_addr = 0; // queue number 0
    } else {
        *R(diskno, VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number
    }

    // Submit only — completion is handled by virtio_disk_intr() which
    // frees the descriptor chain and signals the bio via bio_complete().
    // Callers wait for I/O via bio_await().
    disk->info[idx[0]].done = false;
    spin_unlock(&disk->vdisk_lock);
}

static void virtio_disk_intr(int irq, void *data, device_t *dev) {
    uint64 diskno = (uint64)data;
    struct disk *disk = &disks[diskno];
    spin_lock(&disk->vdisk_lock);

    // Acknowledge the interrupt: PCI vs MMIO transport
    if (disk->pci_state.use_pci) {
        // PCI: read ISR status register to acknowledge interrupt
        // Bit 0 = used buffer notification, bit 1 = config change
        volatile uint8 isr_status = *disk->pci_state.isr;
        (void)isr_status;
    } else {
        *R(diskno, VIRTIO_MMIO_INTERRUPT_ACK) =
            *R(diskno, VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // the device increments disk->used->idx when it
    // adds an entry to the used ring.

    while (disk->used_idx != disk->used->idx) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        int id = disk->used->ring[disk->used_idx % NUM].id;

        char status = disk->info[id].status;

        struct bio *bio = disk->info[id].bio;

        if (status != 0) {
            printf("ERROR: id=%d status=%d buf=%p blockno=0x%lx\n", id, status,
                   bio, bio ? bio->blkno : 0);
            panic("virtio_disk_intr status: %d", status);
        }

        assert(disk->info[id].done == false, "virtio_disk_intr: already done");
        disk->info[id].done = true;

        // Clean up descriptor chain and info slot
        disk->info[id].bio = NULL;
        free_chain(disk, id);

        // Signal bio completion — wakes any thread in bio_await()
        if (bio != NULL) {
            bio->error = (status != 0) ? -EIO : 0;
            bio_complete(bio);
        }

        disk->used_idx += 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    spin_unlock(&disk->vdisk_lock);
}
