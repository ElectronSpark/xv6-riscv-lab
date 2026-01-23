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
#include "hlist_type.h"
#include "bintree.h"
#include "list_type.h"

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

struct fdt_prop {
    uint32 len;
    uint32 nameoff;
    // followed by property value
} __PACKED;

// FDT node structure for internal representation
// Structure:
// | fd_node | date[0] ... data[n] | name string |
struct fdt_node {
    struct rb_node rb_entry;    // Link to the parent node
    list_node_t list_entry;     // Link all fdt nodes
    struct {
        uint64 phandle: 32;
        uint64 data_size: 16;   // Size of property data in bytes
        uint64 name_size: 16;   // Size of property name in bytes
        uint64 layer: 8;        // Layer in the tree
        uint64 fdt_type: 4;
        uint64 has_addr: 1;
        uint64 has_phandle: 1;
        uint64 truncated: 1;
    };
    ht_hash_t hash;          // Hash of the node name
    int child_count;    // Number of child nodes
    struct rb_root children;  // Properties/sub-nodes in this node
    uint64 addr;        // The unit address (from name@addr)
    const char *name;
    uint32 data[0];  // Property data follows
};

// parsed and reconstructed FDT blob info
struct fdt_blob_info {
    struct fdt_header original_header; // Original FDT header
    struct rb_root root;       // Root node
    list_node_t all_nodes;     // All nodes list
    int n_nodes;            // Number of nodes
    uint32 boot_cpuid_phys;

    // Reserved memory regions (from /memreserve/ and /reserved-memory)
    struct mem_region *reserved;
    int reserved_count;
};

// Probed platform information
struct platform_info {
    // Memory regions (may have multiple banks)
    struct mem_region mem[MAX_MEM_REGIONS];
    int mem_count;
    
    // Reserved memory regions (from /memreserve/ and /reserved-memory)
    struct mem_region *reserved;
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

// Early boot: quickly find memory region without full FDT parsing
// Returns 0 on success, -1 on failure
// This is a lightweight linear scan used before full FDT tree is built
int fdt_early_scan_memory(void *dtb, uint64 *base_out, uint64 *size_out);

// Initialize FDT parser and probe platform info
// dtb: pointer to the device tree blob (passed by bootloader in a1)
int fdt_init(void *dtb);

// Validate FDT header
int fdt_valid(void *dtb);

// Get total size of FDT
uint32 fdt_totalsize(void *dtb);

// Debug: dump FDT structure
void fdt_dump(void *dtb);

// Walk and print all FDT entries (nodes, properties, values)
// Useful for debugging and discovering device tree contents
void fdt_walk(void *dtb);

// Lookup a child node by name and optional unit address
// If addr is NULL, it will try to parse the unit address from the namestring
// If addr is provided, it will override any unit address in the namestring
struct fdt_node *fdt_node_lookup(struct fdt_node *parent,
                                 const char *name, uint64 *addr);

// Lookup a node by path (e.g., "/cpus/cpu@0" or "/soc/uart@10000000")
// Returns the matching fdt_node, or NULL if not found
// Path must start with '/' for absolute paths
struct fdt_node *fdt_path_lookup(struct fdt_blob_info *blob,
                                 const char *path);

#endif /* __KERNEL_FDT_H */
