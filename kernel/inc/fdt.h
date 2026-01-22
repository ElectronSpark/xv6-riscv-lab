/**
 * Flattened Device Tree (FDT) Parser
 * 
 * Simple parser to extract device addresses from the DTB passed by
 * the bootloader. This allows runtime detection of hardware instead
 * of compile-time constants.
 */

#ifndef __KERNEL_FDT_H
#define __KERNEL_FDT_H

#include "compiler.h"
#include "types.h"

// FDT header magic number
#define FDT_MAGIC       0xd00dfeed

// FDT token types
#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

// FDT header structure
struct fdt_header {
    uint32 magic;
    uint32 totalsize;
    uint32 off_dt_struct;
    uint32 off_dt_strings;
    uint32 off_mem_rsvmap;
    uint32 version;
    uint32 last_comp_version;
    uint32 boot_cpuid_phys;
    uint32 size_dt_strings;
    uint32 size_dt_struct;
} __PACKED;

#define MAX_MEM_REGIONS 8
#define MAX_RESERVED_REGIONS 16

// Memory region
struct mem_region {
    uint64 base;
    uint64 size;
} __PACKED;

// Probed platform information
struct platform_info {
    // Memory regions (may have multiple banks)
    struct mem_region mem[MAX_MEM_REGIONS];
    int mem_count;
    
    // Reserved memory regions (from /memreserve/ and /reserved-memory)
    struct mem_region reserved[MAX_RESERVED_REGIONS];
    int reserved_count;
    
    // Ramdisk region (pre-loaded filesystem image)
    uint64 ramdisk_base;
    uint64 ramdisk_size;
    int has_ramdisk;
    
    // Total memory (sum of all regions)
    uint64 total_mem;
    
    // UART
    uint64 uart_base;
    uint32 uart_irq;
    
    // PLIC
    uint64 plic_base;
    uint64 plic_size;
    
    // VirtIO (if present)
    int has_virtio;
    uint64 virtio_base[8];
    uint32 virtio_irq[8];
    int virtio_count;
    
    // Timebase frequency
    uint64 timebase_freq;
    
    // Number of CPUs
    int ncpu;
};

// Global platform info (populated by fdt_init)
extern struct platform_info platform;

// Initialize FDT parser and probe platform info
// dtb: pointer to the device tree blob (passed by bootloader in a1)
int fdt_init(void *dtb);

// Validate FDT header
int fdt_valid(void *dtb);

// Get total size of FDT
uint32 fdt_totalsize(void *dtb);

// Debug: dump FDT structure
void fdt_dump(void *dtb);

#endif /* __KERNEL_FDT_H */
