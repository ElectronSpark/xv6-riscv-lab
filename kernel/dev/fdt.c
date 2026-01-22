/**
 * Flattened Device Tree (FDT) Parser
 * 
 * Parses the device tree blob passed by the bootloader to extract
 * hardware information at runtime.
 */

#include "types.h"
#include "fdt.h"
#include "bits.h"
#include "printf.h"
#include "string.h"

// Global platform info - populated by fdt_init()
// Code can access platform.uart_base, platform.plic_base, etc. for runtime values
struct platform_info platform;

// Byte-swap for big-endian FDT format
#define fdt32_to_cpu(x) bswap32(x)
#define fdt64_to_cpu(x) bswap64(x)

// Get FDT header field
#define fdt_get_header(dtb, offset) fdt32_to_cpu(*(uint32*)((char*)dtb + offset))

// Validate FDT header
#define fdt_valid(dtb) ((dtb) && fdt_get_header(dtb, 0) == FDT_MAGIC)

uint32 fdt_totalsize(void *dtb) {
    return fdt_get_header(dtb, 4);
}

// Align to 4-byte boundary
static inline uint32 fdt_align(uint32 x) {
    return (x + 3) & ~3;
}

// Get string from strings block
static const char* fdt_get_string(void *dtb, uint32 offset) {
    uint32 str_off = fdt_get_header(dtb, 12);  // off_dt_strings
    return (const char*)dtb + str_off + offset;
}

// Compare node name (handles unit address like "serial@d4017000")
static int fdt_node_match(const char *name, const char *pattern) {
    while (*pattern) {
        if (*name != *pattern)
            return 0;
        name++;
        pattern++;
    }
    // Pattern matched; allow trailing @... in name
    return (*name == '\0' || *name == '@');
}

// Check if node name has a unit address (@xxx)
static int fdt_node_has_addr(const char *name) {
    while (*name) {
        if (*name == '@')
            return 1;
        name++;
    }
    return 0;
}

// Parse a reg property to get base address
// Returns the first address in the reg property
static uint64 fdt_parse_reg(void *data, int len, int addr_cells, int size_cells) {
    uint32 *cells = (uint32*)data;
    uint64 addr = 0;
    
    if (addr_cells == 2) {
        addr = ((uint64)fdt32_to_cpu(cells[0]) << 32) | fdt32_to_cpu(cells[1]);
    } else {
        addr = fdt32_to_cpu(cells[0]);
    }
    
    return addr;
}

// Parse interrupts property
static uint32 fdt_parse_irq(void *data, int len) {
    uint32 *cells = (uint32*)data;
    return fdt32_to_cpu(cells[0]);
}

// Parse the memory reservation map (pairs of address, size until both are 0)
static void fdt_parse_memreserve(void *dtb) {
    uint32 rsvmap_off = fdt_get_header(dtb, 16);  // off_mem_rsvmap
    uint64 *rsvmap = (uint64 *)((char *)dtb + rsvmap_off);
    
    while (1) {
        uint64 addr = fdt64_to_cpu(rsvmap[0]);
        uint64 size = fdt64_to_cpu(rsvmap[1]);
        
        if (addr == 0 && size == 0)
            break;
            
        if (platform.reserved_count < MAX_RESERVED_REGIONS) {
            platform.reserved[platform.reserved_count].base = addr;
            platform.reserved[platform.reserved_count].size = size;
            platform.reserved_count++;
            printf("fdt: memreserve 0x%lx - 0x%lx (%ld KB)\n", 
                   addr, addr + size, size / 1024);
        }
        
        rsvmap += 2;
    }
}

// FDT parsing state
struct fdt_state {
    void *dtb;
    char *struct_start;
    char *strings;
    int depth;
    int addr_cells;
    int size_cells;
    char path[256];
    int path_len;
};

// Parse the FDT structure
static void fdt_parse(struct fdt_state *state) {
    char *p = state->struct_start;
    uint32 token;
    
    // Stack for #address-cells and #size-cells at each depth
    // Default to 2 cells each (common for 64-bit platforms)
    int addr_stack[16] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    int size_stack[16] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    int depth = 0;
    
    // Track current node for property parsing
    char current_node[64] = "";
    int in_cpus = 0;
    int in_memory = 0;
    int in_reserved_memory = 0;  // Inside /reserved-memory node
    int in_chosen = 0;           // Inside /chosen node
    int is_plic = 0;  // Current node is PLIC
    int cpu_count = 0;
    
    while (1) {
        token = fdt32_to_cpu(*(uint32*)p);
        p += 4;
        
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = p;
            int namelen = strlen(name);
            p += fdt_align(namelen + 1);
            
            depth++;
            if (depth < 16) {
                addr_stack[depth] = addr_stack[depth-1];
                size_stack[depth] = size_stack[depth-1];
            }
            
            strncpy(current_node, name, sizeof(current_node)-1);
            
            // Track which section we're in
            // Root node "" is at depth 1, so cpus/memory are at depth 2
            if (depth == 2) {
                in_cpus = fdt_node_match(name, "cpus");
                in_memory = fdt_node_match(name, "memory");
                in_reserved_memory = fdt_node_match(name, "reserved-memory");
                in_chosen = fdt_node_match(name, "chosen");
            }
            if (in_cpus && depth == 3 && fdt_node_match(name, "cpu")) {
                cpu_count++;
            }
            break;
        }
        
        case FDT_END_NODE:
            if (depth == 2) {
                in_cpus = 0;
                in_memory = 0;
                in_reserved_memory = 0;
                in_chosen = 0;
            }
            is_plic = 0;
            depth--;
            current_node[0] = '\0';
            break;
            
        case FDT_PROP: {
            uint32 len = fdt32_to_cpu(*(uint32*)p);
            p += 4;
            uint32 nameoff = fdt32_to_cpu(*(uint32*)p);
            p += 4;
            
            const char *propname = fdt_get_string(state->dtb, nameoff);
            void *data = p;
            p += fdt_align(len);
            
            // Parse #address-cells and #size-cells
            if (strncmp(propname, "#address-cells", 15) == 0 && len == 4) {
                if (depth < 16)
                    addr_stack[depth] = fdt32_to_cpu(*(uint32*)data);
            }
            if (strncmp(propname, "#size-cells", 12) == 0 && len == 4) {
                if (depth < 16)
                    size_stack[depth] = fdt32_to_cpu(*(uint32*)data);
            }
            
            // Parse timebase-frequency in /cpus node (at depth 2)
            if (in_cpus && depth == 2 && strncmp(propname, "timebase-frequency", 19) == 0) {
                if (len == 4)
                    platform.timebase_freq = fdt32_to_cpu(*(uint32*)data);
                else if (len == 8)
                    platform.timebase_freq = fdt64_to_cpu(*(uint64*)data);
            }
            
            // Parse /chosen node properties for initrd (ramdisk)
            if (in_chosen && depth == 2) {
                // linux,initrd-start - address where initrd begins
                if (strncmp(propname, "linux,initrd-start", 19) == 0) {
                    if (len == 4)
                        platform.ramdisk_base = fdt32_to_cpu(*(uint32*)data);
                    else if (len == 8)
                        platform.ramdisk_base = fdt64_to_cpu(*(uint64*)data);
                    printf("fdt: chosen linux,initrd-start = 0x%lx\n", platform.ramdisk_base);
                }
                // linux,initrd-end - address where initrd ends
                // Store in ramdisk_size temporarily, will calculate actual size later
                if (strncmp(propname, "linux,initrd-end", 17) == 0) {
                    if (len == 4)
                        platform.ramdisk_size = fdt32_to_cpu(*(uint32*)data);
                    else if (len == 8)
                        platform.ramdisk_size = fdt64_to_cpu(*(uint64*)data);
                    printf("fdt: chosen linux,initrd-end = 0x%lx\n", platform.ramdisk_size);
                }
            }
            
            // Parse memory reg - may contain multiple regions
            if (in_memory && strncmp(propname, "reg", 4) == 0) {
                int ac = addr_stack[depth-1];
                int sc = size_stack[depth-1];
                uint32 *cells = (uint32*)data;
                int total_cells = len / 4;
                int cells_per_entry = ac + sc;
                int num_entries = total_cells / cells_per_entry;
                
                printf("fdt: memory '%s' len=%d ac=%d sc=%d\n", 
                       current_node, len, ac, sc);
                printf("fdt:   raw cells: 0x%x 0x%x 0x%x 0x%x\n",
                       fdt32_to_cpu(cells[0]), fdt32_to_cpu(cells[1]),
                       fdt32_to_cpu(cells[2]), fdt32_to_cpu(cells[3]));
                
                // Parse all memory regions in this reg property
                for (int i = 0; i < num_entries && platform.mem_count < MAX_MEM_REGIONS; i++) {
                    uint32 *entry = cells + (i * cells_per_entry);
                    uint64 base = 0, size = 0;
                    
                    // Parse base address
                    if (ac == 2) {
                        base = ((uint64)fdt32_to_cpu(entry[0]) << 32) | 
                               fdt32_to_cpu(entry[1]);
                    } else {
                        base = fdt32_to_cpu(entry[0]);
                    }
                    
                    // Parse size
                    uint32 size_hi = 0, size_lo = 0;
                    if (sc == 2) {
                        size_hi = fdt32_to_cpu(entry[ac]);
                        size_lo = fdt32_to_cpu(entry[ac + 1]);
                        size = ((uint64)size_hi << 32) | size_lo;
                    } else {
                        size_lo = fdt32_to_cpu(entry[ac]);
                        size = size_lo;
                    }
                    
                    printf("fdt:   region base=0x%lx size=0x%lx (%ld MB)\n",
                           base, size, size / (1024*1024));
                    
                    // Store this region
                    platform.mem[platform.mem_count].base = base;
                    platform.mem[platform.mem_count].size = size;
                    platform.mem_count++;
                    platform.total_mem += size;
                }
            }
            
            // Parse reserved-memory child nodes (at depth 3)
            // These are children of /reserved-memory with reg properties
            if (in_reserved_memory && depth == 3 && strncmp(propname, "reg", 4) == 0) {
                int ac = addr_stack[depth-1];
                int sc = size_stack[depth-1];
                uint32 *cells = (uint32*)data;
                int total_cells = len / 4;
                int cells_per_entry = ac + sc;
                int num_entries = total_cells / cells_per_entry;
                
                // Parse all reserved regions in this reg property
                for (int i = 0; i < num_entries && platform.reserved_count < MAX_RESERVED_REGIONS; i++) {
                    uint32 *entry = cells + (i * cells_per_entry);
                    uint64 base = 0, size = 0;
                    
                    // Parse base address
                    if (ac == 2) {
                        base = ((uint64)fdt32_to_cpu(entry[0]) << 32) | 
                               fdt32_to_cpu(entry[1]);
                    } else {
                        base = fdt32_to_cpu(entry[0]);
                    }
                    
                    // Parse size
                    if (sc == 2) {
                        size = ((uint64)fdt32_to_cpu(entry[ac]) << 32) |
                               fdt32_to_cpu(entry[ac + 1]);
                    } else {
                        size = fdt32_to_cpu(entry[ac]);
                    }
                    
                    printf("fdt: reserved-memory '%s' 0x%lx - 0x%lx (%ld KB)\n",
                           current_node, base, base + size, size / 1024);
                    
                    // Store this reserved region
                    platform.reserved[platform.reserved_count].base = base;
                    platform.reserved[platform.reserved_count].size = size;
                    platform.reserved_count++;
                }
            }
            
            // Parse compatible to identify device type, then parse reg/interrupts
            if (depth >= 1 && strncmp(propname, "compatible", 11) == 0) {
                // Check for known device types (use bounded search within property length)
                if (strstr((char*)data, "ns16550") || 
                    strstr((char*)data, "uart") ||
                    strstr((char*)data, "serial") ||
                    strstr((char*)data, "pxa-uart")) {
                    // This is a UART - we'll get reg from the node name
                    // or wait for the reg property
                }
                if (strstr((char*)data, "plic") ||
                    strstr((char*)data, "riscv,plic")) {
                    // Mark this node as PLIC for reg parsing
                    is_plic = 1;
                }
                if (strstr((char*)data, "virtio")) {
                    // VirtIO device
                    platform.has_virtio = 1;
                }
            }
            
            // Parse reg property for known device nodes
            if (strncmp(propname, "reg", 4) == 0 && depth >= 1) {
                int ac = addr_stack[depth-1];
                int sc = size_stack[depth-1];
                uint64 addr = fdt_parse_reg(data, len, ac, sc);
                
                // Identify device by node name pattern
                if (fdt_node_match(current_node, "serial") ||
                    fdt_node_match(current_node, "uart")) {
                    if (platform.uart_base == 0) {
                        platform.uart_base = addr;
                    }
                }
                // PLIC: detect by is_plic flag OR by node name "plic@" or 
                // "interrupt-controller@" (with unit addr, not CPU-local intc)
                if ((is_plic || fdt_node_match(current_node, "plic") ||
                     (fdt_node_match(current_node, "interrupt-controller") && 
                      fdt_node_has_addr(current_node))) &&
                    platform.plic_base == 0) {
                    platform.plic_base = addr;
                    // Also get size based on #size-cells
                    uint32 *cells = (uint32*)data;
                    int total_cells = len / 4;
                    if (ac == 2 && sc == 2 && total_cells >= 4) {
                        platform.plic_size = ((uint64)fdt32_to_cpu(cells[2]) << 32) |
                                             fdt32_to_cpu(cells[3]);
                    } else if (ac == 2 && sc == 1 && total_cells >= 3) {
                        platform.plic_size = fdt32_to_cpu(cells[2]);
                    }
                }
                if (fdt_node_match(current_node, "virtio") ||
                    fdt_node_match(current_node, "virtio_mmio")) {
                    if (platform.virtio_count < 8) {
                        platform.virtio_base[platform.virtio_count] = addr;
                        platform.virtio_count++;
                    }
                }
            }
            
            // Parse interrupts property
            if (strncmp(propname, "interrupts", 11) == 0 && depth >= 1) {
                uint32 irq = fdt_parse_irq(data, len);
                
                if (fdt_node_match(current_node, "serial") ||
                    fdt_node_match(current_node, "uart")) {
                    if (platform.uart_irq == 0)
                        platform.uart_irq = irq;
                }
                // VirtIO IRQ
                if ((fdt_node_match(current_node, "virtio") ||
                     fdt_node_match(current_node, "virtio_mmio")) &&
                    platform.virtio_count > 0) {
                    platform.virtio_irq[platform.virtio_count - 1] = irq;
                }
            }
            break;
        }
        
        case FDT_NOP:
            break;
            
        case FDT_END:
            platform.ncpu = cpu_count > 0 ? cpu_count : 1;
            return;
            
        default:
            // Unknown token - stop parsing
            printf("fdt: unknown token 0x%x at depth %d\n", token, depth);
            return;
        }
    }
}

int fdt_init(void *dtb) {
    // Try the provided address first
    printf("fdt: checking DTB at %p\n", dtb);
    
    if (!fdt_valid(dtb)) {
        printf("fdt: no valid DTB found!\n");
        return -1;
    }
    
    printf("fdt: using DTB at %p (size %d bytes)\n", dtb, fdt_totalsize(dtb));
    
    // Initialize platform info with zeros
    memset(&platform, 0, sizeof(platform));
    
    // Parse the memory reservation map first
    fdt_parse_memreserve(dtb);
    
    // Set up parsing state
    struct fdt_state state;
    state.dtb = dtb;
    state.struct_start = (char*)dtb + fdt_get_header(dtb, 8);  // off_dt_struct
    state.strings = (char*)dtb + fdt_get_header(dtb, 12);      // off_dt_strings
    state.depth = 0;
    state.addr_cells = 2;
    state.size_cells = 1;
    state.path[0] = '\0';
    state.path_len = 0;
    
    // Parse the FDT
    fdt_parse(&state);
    
    // Post-process ramdisk: ramdisk_size currently holds the end address
    // Convert to actual size if both start and end were found
    if (platform.ramdisk_base != 0 && platform.ramdisk_size > platform.ramdisk_base) {
        uint64 end = platform.ramdisk_size;
        platform.ramdisk_size = end - platform.ramdisk_base;
        platform.has_ramdisk = 1;
    }
    
    // Print discovered hardware
    printf("fdt: probed platform info:\n");
    printf("  Memory regions: %d (total %ld MB)\n", 
           platform.mem_count, platform.total_mem / (1024*1024));
    for (int i = 0; i < platform.mem_count; i++) {
        printf("    [%d] 0x%lx - 0x%lx (%ld MB)\n", i,
               platform.mem[i].base,
               platform.mem[i].base + platform.mem[i].size,
               platform.mem[i].size / (1024*1024));
    }
    printf("  Reserved regions: %d\n", platform.reserved_count);
    if (platform.reserved_count > 0) {
        for (int i = 0; i < platform.reserved_count; i++) {
            printf("    [%d] 0x%lx - 0x%lx (%ld KB)\n", i,
                   platform.reserved[i].base,
                   platform.reserved[i].base + platform.reserved[i].size,
                   platform.reserved[i].size / 1024);
        }
    }
    if (platform.has_ramdisk) {
        printf("  Ramdisk: 0x%lx - 0x%lx (%ld KB)\n",
               platform.ramdisk_base,
               platform.ramdisk_base + platform.ramdisk_size,
               platform.ramdisk_size / 1024);
    }
    printf("  UART: 0x%lx, IRQ %d\n", platform.uart_base, platform.uart_irq);
    printf("  PLIC: 0x%lx\n", platform.plic_base);
    printf("  CPUs: %d, timebase: %ld Hz\n", platform.ncpu, platform.timebase_freq);
    
    if (platform.has_virtio) {
        printf("  VirtIO devices: %d\n", platform.virtio_count);
        for (int i = 0; i < platform.virtio_count; i++) {
            printf("    [%d] 0x%lx, IRQ %d\n", i, 
                   platform.virtio_base[i], platform.virtio_irq[i]);
        }
    }
    
    return 0;
}

void fdt_dump(void *dtb) {
    if (!fdt_valid(dtb)) {
        printf("fdt_dump: invalid DTB\n");
        return;
    }
    struct fdt_header *header = dtb;
    
    printf("FDT at %p:\n", dtb);
    printf("  magic: 0x%x\n", fdt32_to_cpu(header->magic));
    printf("  totalsize: %d\n", fdt32_to_cpu(header->totalsize));
    printf("  off_dt_struct: 0x%x\n", fdt32_to_cpu(header->off_dt_struct));
    printf("  off_dt_strings: 0x%x\n", fdt32_to_cpu(header->off_dt_strings));
    printf("  version: %d\n", fdt32_to_cpu(header->version));
}
