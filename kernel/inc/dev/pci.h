// This is from the PCI-E Base 4.0 Specification Revision 0.3
#ifndef __KERNEL_PCI_H_
#define __KERNEL_PCI_H_

#include "compiler.h"

extern uint64 __pcie_ecam_mmio_base;
#define PCIE_ECAM __pcie_ecam_mmio_base

// PCI-E Common Configuration Space Header Structure
struct pci_common_confspace_header {
    uint16 vendor_id;
    uint16 device_id;
    uint16 command;
    uint16 status;
    struct {
        uint32 revision_id : 8;
        uint32 class_code : 24;
    };
    uint8 cache_line_size;
    uint8 master_latency_timer;
    uint8 header_type;
    uint8 bist;
    union {
        uint32 header_type_spec[12];
        struct {
            uint8 header_type_spec_0[36];
            uint8 caps_ptr; // capabilities pointer
            uint8 header_type_spec_1[7];
            uint8 intr_line; // interrupt line
            uint8 intr_pin;  // interrupt pin
            uint8 header_type_spec_2[2];
        } header_type_common;
        struct {
            uint32 base_addr[6]; // base address
            uint32 card_bus_cis_ptr;
            uint16 subsys_vendor_id; // subsystem vendor ID
            uint16 subsys_id;        // subsystem ID
            uint32 eprom_base_addr;  // expansion rom base address
            uint8 caps_ptr;          // capabilities pointer
            uint8 rsvd[7];           // reserved
            uint8 intr_line;         // interrupt line
            uint8 intr_pin;          // interrupt pin
            uint8 min_gnt;
            uint8 min_lat;
        } header_type_0;
        struct {
            uint32 base_addr_reg0;   // base address register 0
            uint32 base_addr_reg1;   // base address register 1
            uint8 pri_bus_no;        // primary bus number
            uint8 sec_bus_no;        // secondary bus number
            uint8 sub_bus_no;        // subordinate bus number
            uint8 sec_lat_timer;     // secondary latency timer
            uint8 io_base;           // I/O base lower 8 bits
            uint8 io_limit;          // I/O limit lower 8 bits
            uint16 sstatus;          // secondary status
            uint16 mem_base;         // memory base
            uint16 mem_limit;        // memory limit
            uint16 pmem_base;        // prefetchable memory base lower 16bits
            uint16 pmem_limit;       // prefetchable memory limit lower 16bits
            uint32 pmem_base_upper;  // prefetchable memory base upper 32bits
            uint32 pmem_limit_upper; // prefetchable memory limit upper 32bits
            uint16 io_base_upper;    // I/O base upper 16 bits
            uint16 io_limit_upper;   // I/O limit upper 16 bits
            uint8 caps_ptr;          // capabilities pointer
            uint8 rsvd[7];           // reserved
            uint8 intr_line;         // interrupt line
            uint8 intr_pin;          // interrupt pin
            uint16 bridge_ctl;
        } header_type_1;
    };
};

// PCI 3.0 Configuration Space Command
#define PCIE_CSCMD_IAE (1U << 0)           // bit 0 - I/O Access Enable - ?
#define PCIE_CSCMD_MAE (1U << 1)           // bit 1 - Memory Access Enable - ?
#define PCIE_CSCMD_BME (1U << 2)           // bit 2 - BUS Master Enable - RW
#define PCIE_CSCMD_PER (1U << 6)           // bit 6 - Parity Error Response - RW
#define PCIE_CSCMD_SEER_ENABLE (1U << 8)   // bit 8 - SEER ENABLE - RW
#define PCIE_CSCMD_INTR_DISABLE (1U << 10) // bit 10 - Interrupt Disable - RW

// PCI 3.0 Status Register Bits
#define PCIE_STATUS_INTR (1U << 3)    // bit 3 - Interrupt Status
#define PCIE_STATUS_CAPL (1U << 4)    // bit 4 - Capabilities List
#define PCIE_STATUS_CAP66MZ (1U << 5) // bit 5 - 66 MHz Capable
#define PCIE_STATUS_CAPB2BT                                                    \
    (1U << 7) // bit 7 - Fast Back-to-Back Transactions Capable
#define PCIE_STATUS_MDPE (1U << 8) // bit 8 - Master Data Parity Error - RW
#define PCIE_STATUS_DEVSEL_TMASK (3U << 9) // bit 9,10 - DEVSEL Timing
#define PCIE_STATUS_STA (1U << 11)         // bit 11 - Signaled Target Abort
#define PCIE_STATUS_RTA (1U << 12)         // bit 12 - Received Target Abort
#define PCIE_STATUS_SMA (1U << 13)         // bit 13 - Received Master Abort
#define PCIE_STATUS_SSE (1U << 14)         // bit 14 - Signaled System Error
#define PCIE_STATUS_DPE (1U << 15)         // bit 15 - Detected Parity Error

// PCI 3.0 Header Type Register
#define PCIE_HEADER_TYPE_MFD (1U << 7) // bit 7 - Multi-Function Device

// PCI 3.0 Interrupt Pin Register
#define PCIE_INTR_PIN_NONE 0x00 // indicates legacy interrupt Messages were used
#define PCIE_INTR_PIN_INTA 0x01 // legacy interrupt Message INTA
#define PCIE_INTR_PIN_INTB 0x02 // legacy interrupt Message INTB
#define PCIE_INTR_PIN_INTC 0x03 // legacy interrupt Message INTC
#define PCIE_INTR_PIN_INTD 0x04 // legacy interrupt Message INTD

// PCI Vendor/Device IDs
#define PCI_VENDOR_INTEL    0x8086
#define PCI_DEVICE_E1000    0x100e
#define PCI_VENDOR_VIRTIO   0x1AF4
#define PCI_VENDOR_MICROSOFT 0x1414
#define PCI_DEVICE_MS_VIRTUAL_RENDER 0x008e
#define PCI_DEVICE_MS_COMPUTE_ACCELERATOR 0x008a
#define PCI_DEVICE_VIRTIO_BLK_TRANSITIONAL 0x1001 // virtio block (transitional)
#define PCI_DEVICE_VIRTIO_NET_TRANSITIONAL 0x1000 // virtio net (transitional)
#define PCI_DEVICE_VIRTIO_GPU_TRANSITIONAL 0x1010 // virtio gpu (transitional)
#define PCI_DEVICE_VIRTIO_INPUT_TRANSITIONAL 0x1012 // virtio input (transitional)
#define PCI_DEVICE_VIRTIO_BLK_MODERN       0x1042 // virtio block (modern only)
#define PCI_DEVICE_VIRTIO_NET_MODERN       0x1041 // virtio net (modern only)
#define PCI_DEVICE_VIRTIO_GPU_MODERN       0x1050 // virtio gpu (modern only)
#define PCI_DEVICE_VIRTIO_INPUT_MODERN     0x1052 // virtio input (modern only)

// PCI Capability IDs
#define PCI_CAP_ID_VENDOR   0x09   // Vendor-specific capability (used by virtio)

// Virtio PCI capability types (within vendor cap)
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5

// Virtio PCI capability structure
struct virtio_pci_cap {
    uint8	cap_vndr;       // Generic PCI field: PCI_CAP_ID_VENDOR
    uint8	cap_next;       // Generic PCI field: next ptr
    uint8	cap_len;        // Generic PCI field: capability length
    uint8	cfg_type;       // Identifies the structure (VIRTIO_PCI_CAP_*)
    uint8	bar;            // BAR index [0..5]
    uint8	padding[3];     // Padding
    uint32	offset;         // Offset within bar
    uint32	length;         // Length of the structure, in bytes
};

// Virtio PCI notify capability (extends virtio_pci_cap)
struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    uint32 notify_off_multiplier; // Multiplier for queue_notify_off
};

// Virtio PCI common configuration structure (mapped from BAR)
struct virtio_pci_common_cfg {
    uint32 device_feature_select;  // 0x00 - RW
    uint32 device_feature;         // 0x04 - RO
    uint32 driver_feature_select;  // 0x08 - RW
    uint32 driver_feature;         // 0x0C - RW
    uint16 msix_config;            // 0x10 - RW
    uint16 num_queues;             // 0x12 - RO
    uint8  device_status;          // 0x14 - RW
    uint8  config_generation;      // 0x15 - RO
    uint16 queue_select;           // 0x16 - RW
    uint16 queue_size;             // 0x18 - RW
    uint16 queue_msix_vector;      // 0x1A - RW
    uint16 queue_enable;           // 0x1C - RW
    uint16 queue_notify_off;       // 0x1E - RO
    uint64 queue_desc;             // 0x20 - RW
    uint64 queue_driver;           // 0x28 - RW (avail)
    uint64 queue_device;           // 0x30 - RW (used)
};

// x86 PCI configuration space access via I/O ports 0xCF8/0xCFC
#if defined(__x86_64__) || defined(__i386__)

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static inline void pci_outl(uint16 port, uint32 val)
{
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32 pci_inl(uint16 port)
{
    uint32 val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint32 pci_config_read32(uint8 bus, uint8 dev, uint8 func,
                                       uint8 offset)
{
    uint32 addr = (1U << 31) | ((uint32)bus << 16) | ((uint32)dev << 11) |
                  ((uint32)func << 8) | (offset & 0xFC);
    pci_outl(PCI_CONFIG_ADDR, addr);
    return pci_inl(PCI_CONFIG_DATA);
}

static inline void pci_config_write32(uint8 bus, uint8 dev, uint8 func,
                                      uint8 offset, uint32 val)
{
    uint32 addr = (1U << 31) | ((uint32)bus << 16) | ((uint32)dev << 11) |
                  ((uint32)func << 8) | (offset & 0xFC);
    pci_outl(PCI_CONFIG_ADDR, addr);
    pci_outl(PCI_CONFIG_DATA, val);
}

static inline uint16 pci_config_read16(uint8 bus, uint8 dev, uint8 func,
                                       uint8 offset)
{
    uint32 val = pci_config_read32(bus, dev, func, offset & 0xFC);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

static inline uint8 pci_config_read8(uint8 bus, uint8 dev, uint8 func,
                                     uint8 offset)
{
    uint32 val = pci_config_read32(bus, dev, func, offset & 0xFC);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

static inline void pci_config_write16(uint8 bus, uint8 dev, uint8 func,
                                      uint8 offset, uint16 newval)
{
    uint32 old = pci_config_read32(bus, dev, func, offset & 0xFC);
    int shift = (offset & 2) * 8;
    old &= ~(0xFFFFU << shift);
    old |= ((uint32)newval << shift);
    pci_config_write32(bus, dev, func, offset & 0xFC, old);
}

static inline void pci_config_write8(uint8 bus, uint8 dev, uint8 func,
                                     uint8 offset, uint8 newval)
{
    uint32 old = pci_config_read32(bus, dev, func, offset & 0xFC);
    int shift = (offset & 3) * 8;
    old &= ~(0xFFU << shift);
    old |= ((uint32)newval << shift);
    pci_config_write32(bus, dev, func, offset & 0xFC, old);
}

#endif /* __x86_64__ || __i386__ */

// PCI device info passed to drivers during init
struct pci_device_info {
    uint16 vendor_id;
    uint16 device_id;
    uint8  bus;
    uint8  dev;
    uint8  func;
    uint8  irq_line;  // interrupt line from PCI config
    uint32 bar[6];    // base address registers
};

// Stored info about discovered virtio PCI device
struct virtio_pci_discovery {
    int found;
    uint8 bus, dev, func;
    uint8 irq_line;
    uint32 bar[6];
    // Offsets of virtio PCI caps in config space
    uint8 common_cfg_cap;
    uint8 notify_cfg_cap;
    uint8 isr_cfg_cap;
    uint8 device_cfg_cap;
};

// Get discovered virtio-blk PCI device info by index
struct virtio_pci_discovery *pci_get_virtio_blk(int index);
struct virtio_pci_discovery *pci_get_virtio_gpu(int index);
struct virtio_pci_discovery *pci_get_virtio_input(int index);
struct virtio_pci_discovery *pci_get_virtio_net(int index);

// Prototype
void pci_init(void);

#endif
