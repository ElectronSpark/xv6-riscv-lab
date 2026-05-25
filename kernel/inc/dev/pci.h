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
#define PCI_VENDOR_NVIDIA   0x10DE
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
#define PCI_CAP_ID_MSI      0x05   // Message Signaled Interrupts
#define PCI_CAP_ID_VENDOR   0x09   // Vendor-specific capability (used by virtio)
#define PCI_CAP_ID_MSIX     0x11   // MSI-X

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

struct pci_config_backend_ops {
    int (*read)(void *ctx, uint32 token, uint16 offset, uint8 size,
                uint32 *value);
    int (*write)(void *ctx, uint32 token, uint16 offset, uint8 size,
                 uint32 value);
};

struct pci_virtual_child {
    uint32 backend_index;
    uint32 backend_token;
    uint16 vendor_id;
    uint16 device_id;
    uint32 class_code;
    uint8 revision_id;
    uint8 header_type;
    uint16 subsystem_vendor_id;
    uint16 subsystem_id;
    uint8 irq_line;
};

#define PCI_ANY_ID      0xffffffffU
#define PCI_ANY_CLASS   0xffffffffU

#define PCI_CLASS_DISPLAY               0x03
#define PCI_CLASS_DISPLAY_VGA           0x030000
#define PCI_CLASS_PROCESSING_ACCELERATOR 0x12

#define PCI_RESOURCE_IO         0x0001
#define PCI_RESOURCE_MEM        0x0002
#define PCI_RESOURCE_PREFETCH   0x0004
#define PCI_RESOURCE_MEM_64     0x0008

#define PCI_IRQ_LEGACY 0x1
#define PCI_IRQ_MSI    0x2
#define PCI_IRQ_MSIX   0x4

#define PCI_DMA_BIDIRECTIONAL 0
#define PCI_DMA_TO_DEVICE     1
#define PCI_DMA_FROM_DEVICE   2

struct pci_device_id {
    uint32 vendor;
    uint32 device;
    uint32 subvendor;
    uint32 subdevice;
    uint32 class;
    uint32 class_mask;
    uint64 driver_data;
};

struct pci_driver;

// PCI device info passed to drivers during init
struct pci_device_info {
    uint16 vendor_id;
    uint16 device_id;
    uint16 subsystem_vendor_id;
    uint16 subsystem_id;
    uint8  bus;
    uint8  dev;
    uint8  func;
    uint8  revision_id;
    uint8  irq_line;  // interrupt line from PCI config
    uint8  irq_pin;
    uint8  header_type;
    uint8  enabled;
    uint8  master_enabled;
    uint8  runtime_suspended;
    uint8  irq_vectors_allocated;
    uint8  irq_vector_count;
    uint8  irq_flags;
    uint8  dma_mask_requested_bits;
    uint8  coherent_dma_mask_requested_bits;
    uint8  dma_mask_bits;
    uint8  coherent_dma_mask_bits;
    uint8  dma_mask_configured;
    uint8  coherent_dma_mask_configured;
    uint8  capabilities;
    uint8  msi_cap;
    uint8  msix_cap;
    uint32 class_code;
    uint32 enable_count;
    uint32 suspend_count;
    uint32 resume_count;
    uint32 bar[6];    // base address registers
    uint64 resource_start[6];
    uint64 resource_len[6];
    uint32 resource_flags[6];
    uint8 resource_claimed[6];
    const char *resource_owner[6];
    uint32 resource_claim_count;
    uint32 resource_release_count;
    uint32 resource_iomap_count;
    uint32 resource_owner_mismatch_count;
    uint32 resource_unclaimed_iomap_count;
    uint32 resource_unclaimed_release_count;
    uint32 dma_mask_fallback_32_count;
    uint32 coherent_dma_mask_fallback_32_count;
    uint32 irq_alloc_request_count;
    uint32 irq_alloc_failure_count;
    uint32 irq_msi_request_count;
    uint32 irq_msi_unsupported_count;
    uint32 irq_msix_request_count;
    uint32 irq_msix_unsupported_count;
    uint32 irq_legacy_request_count;
    uint32 irq_legacy_grant_count;
    uint32 dma_map_count;
    uint32 dma_unmap_count;
    uint32 dma_map_fail_count;
    uint64 dma_map_last_va;
    uint64 dma_map_last_dma;
    uint64 dma_map_last_size;
    uint32 dma_map_last_direction;
    int32 dma_map_last_ret;
    void *driver_data;
    const char *driver_name;
    struct pci_driver *driver;
};

#define pci_dev pci_device_info

struct pci_driver {
    const char *name;
    const struct pci_device_id *id_table;
    int (*probe)(struct pci_device_info *pdev,
                 const struct pci_device_id *id);
    void (*remove)(struct pci_device_info *pdev);
    int (*suspend)(struct pci_device_info *pdev);
    int (*resume)(struct pci_device_info *pdev);
    uint32 bound_devices;
};

uint32 pci_config_read32(uint8 bus, uint8 dev, uint8 func, uint16 offset);
uint16 pci_config_read16(uint8 bus, uint8 dev, uint8 func, uint16 offset);
uint8 pci_config_read8(uint8 bus, uint8 dev, uint8 func, uint16 offset);
int pci_config_try_write32(uint8 bus, uint8 dev, uint8 func, uint16 offset,
                           uint32 val);
void pci_config_write32(uint8 bus, uint8 dev, uint8 func, uint16 offset,
                        uint32 val);
void pci_config_write16(uint8 bus, uint8 dev, uint8 func, uint16 offset,
                        uint16 val);
void pci_config_write8(uint8 bus, uint8 dev, uint8 func, uint16 offset,
                       uint8 val);

int pci_register_config_backend(const char *name,
                                const struct pci_config_backend_ops *ops,
                                void *ctx);
int pci_register_virtual_child(const struct pci_virtual_child *child,
                               uint8 *bus_out, uint8 *dev_out,
                               uint8 *func_out);
int pci_probe_virtual_bdf(uint8 bus, uint8 dev, uint8 func);
void pci_probe_registered_virtual_children(void);

int pci_register_driver(struct pci_driver *driver);
void pci_unregister_driver(struct pci_driver *driver);
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         const struct pci_device_info *pdev);
void pci_set_drvdata(struct pci_device_info *pdev, void *data);
void *pci_get_drvdata(const struct pci_device_info *pdev);
uint64 pci_resource_start(const struct pci_device_info *pdev, int bar);
uint64 pci_resource_len(const struct pci_device_info *pdev, int bar);
uint32 pci_resource_flags(const struct pci_device_info *pdev, int bar);
int pci_request_region(struct pci_device_info *pdev, int bar,
                       const char *name);
void pci_release_region(struct pci_device_info *pdev, int bar);
uint8 pci_find_capability(const struct pci_device_info *pdev, uint8 cap_id);
int pci_has_capability(const struct pci_device_info *pdev, uint8 cap_id);
void *pci_iomap(struct pci_device_info *pdev, int bar, uint64 maxlen);
void pci_iounmap(struct pci_device_info *pdev, void *addr);
int pci_enable_device(struct pci_device_info *pdev);
void pci_disable_device(struct pci_device_info *pdev);
void pci_set_master(struct pci_device_info *pdev);
void pci_clear_master(struct pci_device_info *pdev);
int pci_alloc_irq_vectors(struct pci_device_info *pdev, int min_vecs,
                          int max_vecs, uint32 flags);
int pci_irq_vector(struct pci_device_info *pdev, uint32 nr);
void pci_free_irq_vectors(struct pci_device_info *pdev);
int pci_set_dma_mask(struct pci_device_info *pdev, uint64 mask);
int pci_set_consistent_dma_mask(struct pci_device_info *pdev, uint64 mask);
int pci_dma_map_single(struct pci_device_info *pdev, void *cpu_addr,
                       uint64 size, uint32 direction, uint64 *dma_addr);
void pci_dma_unmap_single(struct pci_device_info *pdev, uint64 dma_addr,
                          uint64 size, uint32 direction);
int pci_pm_suspend_device(struct pci_device_info *pdev);
int pci_pm_resume_device(struct pci_device_info *pdev);
void pci_pm_suspend_all(void);
void pci_pm_resume_all(void);

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
struct pci_device_info *pci_get_nvidia_gpu(int index);

// Prototype
void pci_init(void);

#endif
