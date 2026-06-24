//
// virtio device definitions.
// for both the mmio interface, and virtio descriptors.
// only tested with qemu.
//
// the virtio spec:
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
//
#ifndef __KERNEL_VIRTIO_H
#define __KERNEL_VIRTIO_H

#include "compiler.h"

// virtio mmio interface
#define N_VIRTIO_DISK 2 // number of virtio disks
#define N_VIRTIO_GPU 1  // number of virtio GPU devices
#define N_VIRTIO_INPUT 1 // number of virtio input devices
#define N_VIRTIO_NET 1  // number of virtio NICs
#define N_VIRTIO_SND 1  // number of virtio sound devices
#define N_VIRTIO 3      // number of virtio devices
extern uint64 __virtio_mmio_base[N_VIRTIO];
extern uint64 __virtio_irqno[N_VIRTIO];
#define VIRTIO0 __virtio_mmio_base[0]
#define VIRTIO0_IRQ __virtio_irqno[0]
#define VIRTIO1 __virtio_mmio_base[1]
#define VIRTIO1_IRQ __virtio_irqno[1]
#define VIRTIO2 __virtio_mmio_base[2] // virtio console
#define VIRTIO2_IRQ __virtio_irqno[2]

// virtio mmio control registers, mapped starting at 0x10001000.
// from qemu virtio_mmio.h
#define VIRTIO_MMIO_MAGIC_VALUE 0x000 // 0x74726976
#define VIRTIO_MMIO_VERSION 0x004     // version; should be 2
#define VIRTIO_MMIO_DEVICE_ID 0x008   // device type; 1 is net, 2 is disk
#define VIRTIO_MMIO_VENDOR_ID 0x00c   // 0x554d4551
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_QUEUE_SEL 0x030     // select queue, write-only
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034 // max size of current queue, read-only
#define VIRTIO_MMIO_QUEUE_NUM 0x038     // size of current queue, write-only
#define VIRTIO_MMIO_QUEUE_READY 0x044   // ready bit
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050  // write-only
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060 // read-only
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064    // write-only
#define VIRTIO_MMIO_STATUS 0x070           // read/write
#define VIRTIO_MMIO_QUEUE_DESC_LOW                                             \
    0x080 // physical address for descriptor table, write-only
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_DRIVER_DESC_LOW                                            \
    0x090 // physical address for available ring, write-only
#define VIRTIO_MMIO_DRIVER_DESC_HIGH 0x094
#define VIRTIO_MMIO_DEVICE_DESC_LOW                                            \
    0x0a0 // physical address for used ring, write-only
#define VIRTIO_MMIO_DEVICE_DESC_HIGH 0x0a4

// status register bits, from qemu virtio_config.h
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER 2
#define VIRTIO_CONFIG_S_DRIVER_OK 4
#define VIRTIO_CONFIG_S_FEATURES_OK 8

// device feature bits
#define VIRTIO_BLK_F_RO 5          /* Disk is read-only */
#define VIRTIO_BLK_F_SCSI 7        /* Supports scsi command passthru */
#define VIRTIO_BLK_F_FLUSH 9       /* Cache flush command support */
#define VIRTIO_BLK_F_CONFIG_WCE 11 /* Writeback mode available in config */
#define VIRTIO_BLK_F_MQ 12         /* support more than one vq */
#define VIRTIO_F_ANY_LAYOUT 27
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX 29

// this many virtio descriptors.
// must be a power of two.
// 256 allows 85 concurrent I/O ops (each uses 3 descriptors).
#define NUM 256

// a single descriptor, from the spec.
struct virtq_desc {
    uint64 addr;
    uint32 len;
    uint16 flags;
    uint16 next;
};
#define VRING_DESC_F_NEXT 1  // chained with another descriptor
#define VRING_DESC_F_WRITE 2 // device writes (vs read)

// the (entire) avail ring, from the spec.
struct virtq_avail {
    uint16 flags;     // always zero
    uint16 idx;       // driver will write ring[idx] next
    uint16 ring[NUM]; // descriptor numbers of chain heads
    uint16 unused;
};

// one entry in the "used" ring, with which the
// device tells the driver about completed requests.
struct virtq_used_elem {
    uint32 id; // index of start of completed descriptor chain
    uint32 len;
};

struct virtq_used {
    uint16 flags; // always zero
    uint16 idx;   // device increments when it adds a ring[] entry
    struct virtq_used_elem ring[NUM];
};

// these are specific to virtio block devices, e.g. disks,
// described in Section 5.2 of the spec.

#define VIRTIO_BLK_T_IN 0     // read the disk
#define VIRTIO_BLK_T_OUT 1    // write the disk
#define VIRTIO_BLK_T_FLUSH 4  // flush volatile write cache

// virtio block device status values (written by device)
#define VIRTIO_BLK_S_OK 0     // success
#define VIRTIO_BLK_S_IOERR 1  // device or driver error
#define VIRTIO_BLK_S_UNSUPP 2 // unsupported request

// the format of the first descriptor in a disk request.
// to be followed by two more descriptors containing
// the block, and a one-byte status.
struct virtio_blk_req {
    uint32 type; // VIRTIO_BLK_T_IN or ..._OUT
    uint32 reserved;
    uint64 sector;
};

// Per-disk PCI transport state (used on x86 when virtio-pci is detected)
struct virtio_pci_state {
    int use_pci;                           // 1 if PCI transport, 0 if MMIO
    volatile struct virtio_pci_common_cfg *common_cfg;  // mapped BAR region
    volatile uint16 *notify_base;          // mapped notify BAR region
    uint32 notify_off_multiplier;          // from notify cap
    volatile uint8 *isr;                   // mapped ISR BAR region
};

// ─── virtio-net device ───────────────────────────────────────────────
//
// Feature bits (from spec §5.1.3).
#define VIRTIO_NET_F_CSUM               0   /* Host handles partial checksum. */
#define VIRTIO_NET_F_GUEST_CSUM         1   /* Guest handles partial checksum. */
#define VIRTIO_NET_F_MAC                5   /* Host has given a MAC address. */
#define VIRTIO_NET_F_GUEST_TSO4         7   /* Guest can receive TSOv4. */
#define VIRTIO_NET_F_GUEST_TSO6         8   /* Guest can receive TSOv6. */
#define VIRTIO_NET_F_GUEST_UFO          10  /* Guest can receive UFO. */
#define VIRTIO_NET_F_HOST_TSO4          11  /* Host can handle TSOv4 from guest. */
#define VIRTIO_NET_F_HOST_TSO6          12  /* Host can handle TSOv6 from guest. */
#define VIRTIO_NET_F_HOST_UFO           14  /* Host can handle UFO from guest. */
#define VIRTIO_NET_F_MRG_RXBUF          15  /* Driver can merge receive buffers. */
#define VIRTIO_NET_F_STATUS             16  /* Configuration status field is available. */
#define VIRTIO_NET_F_MQ                 22  /* Device supports multiqueue with auto-steering. */
/* Reserved transport feature bit */
#define VIRTIO_F_VERSION_1              32  /* v1.0 compliant. */

/* virtio_net header.  Modern (VERSION_1) layout — always 12 bytes
 * regardless of MRG_RXBUF, since num_buffers is always present. */
struct virtio_net_hdr {
    uint8  flags;
#define VIRTIO_NET_HDR_F_NEEDS_CSUM     1
#define VIRTIO_NET_HDR_F_DATA_VALID     2
    uint8  gso_type;
#define VIRTIO_NET_HDR_GSO_NONE         0
#define VIRTIO_NET_HDR_GSO_TCPV4        1
#define VIRTIO_NET_HDR_GSO_UDP          3
#define VIRTIO_NET_HDR_GSO_TCPV6        4
#define VIRTIO_NET_HDR_GSO_ECN          0x80
    uint16 hdr_len;
    uint16 gso_size;
    uint16 csum_start;
    uint16 csum_offset;
    uint16 num_buffers;     /* present in VERSION_1; ignore unless MRG_RXBUF */
};
_Static_assert(sizeof(struct virtio_net_hdr) == 12,
               "virtio_net_hdr must be 12 bytes (modern layout)");

/* Reserve this much headroom in TX mbufs so the virtio-net driver can
 * mbufpush() the header in place without copying.  Other NICs (e1000)
 * are unaffected — they transmit from m->head/m->len. */
#define VIRTIO_NET_HDR_LEN  12

void virtio_net_init(void);
void virtio_snd_init(void);
int virtio_snd_available(void);
int virtio_snd_write(int user, const void *buf, size_t count, int oss_format,
                     int rate, int channels, int nonblock);
void virtio_snd_reset(void);
void virtio_snd_drain(void);
uint64 virtio_snd_free_bytes(void);
uint64 virtio_snd_pending_bytes(void);
uint64 virtio_snd_played_bytes(void);
uint64 virtio_snd_period_bytes(void);
uint64 virtio_snd_buffer_bytes(void);
int virtio_snd_supported_oss_formats(void);
int virtio_snd_supports_rate(int rate);
const char *virtio_snd_backend_name(void);

#endif /* __KERNEL_VIRTIO_H */
