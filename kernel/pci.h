// This is from the PCI-E Base 4.0 Specification Revision 0.3
#ifndef __KERNEL_PCI_H_
#define __KERNEL_PCI_H_

// PCI-E Common Configuration Space Header Structure
struct pci_common_confspace_header {
    uint16 vendor_id;
    uint16 device_id;
    uint16 command;
    uint16 status;
    struct {
        uint32 revision_id: 8;
        uint32 class_code: 24;
    };
    uint8 cache_line_size;
    uint8 master_latency_timer;
    uint8 header_type;
    uint8 bist;
    union {
        uint32 header_type_spec[12];
        struct {
            uint8 header_type_spec_0[36];
            uint8 caps_ptr;             // capabilities pointer
            uint8 header_type_spec_1[7];
            uint8 intr_line;            // interrupt line
            uint8 intr_pin;             // interrupt pin
            uint8 header_type_spec_2[2];
        } header_type_common;
        struct {
            uint32 base_addr[6];        // base address 
            uint32 card_bus_cis_ptr;
            uint16 subsys_vendor_id;    // subsystem vendor ID
            uint16 subsys_id;           // subsystem ID
            uint32 eprom_base_addr;     // expansion rom base address
            uint8 caps_ptr;             // capabilities pointer
            uint8 rsvd[7];              // reserved
            uint8 intr_line;            // interrupt line
            uint8 intr_pin;             // interrupt pin
            uint8 min_gnt;
            uint8 min_lat;
        } header_type_0;
        struct {
            uint32 base_addr_reg0;      // base address register 0
            uint32 base_addr_reg1;      // base address register 1
            uint8 pri_bus_no;           // primary bus number
            uint8 sec_bus_no;           // secondary bus number
            uint8 sub_bus_no;           // subordinate bus number
            uint8 sec_lat_timer;        // secondary latency timer
            uint8 io_base;              // I/O base lower 8 bits
            uint8 io_limit;             // I/O limit lower 8 bits
            uint16 sstatus;             // secondary status
            uint16 mem_base;            // memory base
            uint16 mem_limit;           // memory limit
            uint16 pmem_base;           // prefetchable memory base lower 16bits
            uint16 pmem_limit;          // prefetchable memory limit lower 16bits
            uint32 pmem_base_upper;     // prefetchable memory base upper 32bits
            uint32 pmem_limit_upper;    // prefetchable memory limit upper 32bits
            uint16 io_base_upper;       // I/O base upper 16 bits
            uint16 io_limit_upper;      // I/O limit upper 16 bits
            uint8 caps_ptr;             // capabilities pointer
            uint8 rsvd[7];              // reserved
            uint8 intr_line;            // interrupt line
            uint8 intr_pin;             // interrupt pin
            uint16 bridge_ctl;
        } header_type_1;
    };
};

// PCI 3.0 Configuration Space Command
#define PCIE_CSCMD_IAE (1U << 0)            // bit 0 - I/O Access Enable - ?
#define PCIE_CSCMD_MAE (1U << 1)            // bit 1 - Memory Access Enable - ?
#define PCIE_CSCMD_BME (1U << 2)            // bit 2 - BUS Master Enable - RW
#define PCIE_CSCMD_PER (1U << 6)            // bit 6 - Parity Error Response - RW
#define PCIE_CSCMD_SEER_ENABLE (1U << 8)    // bit 8 - SEER ENABLE - RW
#define PCIE_CSCMD_INTR_DISABLE (1U << 10)  // bit 10 - Interrupt Disable - RW

// PCI 3.0 Status Register Bits
#define PCIE_STATUS_INTR (1U << 3)          // bit 3 - Interrupt Status
#define PCIE_STATUS_CAPL (1U << 4)          // bit 4 - Capabilities List
#define PCIE_STATUS_CAP66MZ (1U << 5)       // bit 5 - 66 MHz Capable
#define PCIE_STATUS_CAPB2BT (1U << 7)       // bit 7 - Fast Back-to-Back Transactions Capable
#define PCIE_STATUS_MDPE (1U << 8)          // bit 8 - Master Data Parity Error - RW
#define PCIE_STATUS_DEVSEL_TMASK (3U << 9)  // bit 9,10 - DEVSEL Timing
#define PCIE_STATUS_STA (1U << 11)          // bit 11 - Signaled Target Abort
#define PCIE_STATUS_RTA (1U << 12)          // bit 12 - Received Target Abort
#define PCIE_STATUS_SMA (1U << 13)          // bit 13 - Received Master Abort
#define PCIE_STATUS_SSE (1U << 14)          // bit 14 - Signaled System Error
#define PCIE_STATUS_DPE (1U << 15)          // bit 15 - Detected Parity Error

// PCI 3.0 Header Type Register
#define PCIE_HEADER_TYPE_MFD (1U << 7)      // bit 7 - Multi-Function Device

// PCI 3.0 Interrupt Pin Register 
#define PCIE_INTR_PIN_NONE 0x00     // indicates legacy interrupt Messages were used
#define PCIE_INTR_PIN_INTA 0x01     // legacy interrupt Message INTA
#define PCIE_INTR_PIN_INTB 0x02     // legacy interrupt Message INTB
#define PCIE_INTR_PIN_INTC 0x03     // legacy interrupt Message INTC
#define PCIE_INTR_PIN_INTD 0x04     // legacy interrupt Message INTD



#endif