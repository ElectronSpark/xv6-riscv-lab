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
#include "hlist.h"
#include "rbtree.h"
#include "early_allocator.h"

// Global platform info - populated by fdt_init()
// Code can access platform.uart_base, platform.plic_base, etc. for runtime
// values
struct platform_info platform;

// Byte-swap for big-endian FDT format
#define fdt32_to_cpu(x) bswap32(x)
#define fdt64_to_cpu(x) bswap64(x)

// Get FDT header field
#define fdt_get_header(dtb, offset)                                            \
    fdt32_to_cpu(*(uint32 *)((char *)dtb + offset))

// Validate FDT header
#define fdt_valid(dtb) ((dtb) && fdt_get_header(dtb, 0) == FDT_MAGIC)

#define fdt_totalsize(dtb) fdt32_to_cpu(((struct fdt_header *)(dtb))->totalsize)

// Align to 4-byte boundary
#define fdt_align(x) (((x) + 3) & ~3)

// Get string from strings block
static const char *fdt_get_string(void *dtb, uint32 offset) {
    uint32 str_off = fdt_get_header(dtb, 12); // off_dt_strings
    return (const char *)dtb + str_off + offset;
}

// Early boot: quickly scan FDT to find first memory region
// This is a lightweight linear scan - no allocations, no tree building
// Returns 0 on success with base/size filled in, -1 on failure
int fdt_early_scan_memory(void *dtb, uint64 *base_out, uint64 *size_out) {
    if (!fdt_valid(dtb) || !base_out || !size_out) {
        return -1;
    }

    char *struct_start = (char *)dtb + fdt_get_header(dtb, 8);  // off_dt_struct
    char *p = struct_start;
    uint32 token;
    int depth = 0;
    int in_memory_node = 0;
    int root_addr_cells = 2;  // Default
    int root_size_cells = 1;  // Default

    while (1) {
        token = fdt32_to_cpu(*(uint32 *)p);
        p += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = p;
            int namelen = strlen(name);
            p += fdt_align(namelen + 1);

            depth++;
            // Check if this is a memory node (name starts with "memory")
            if (depth == 1 && strncmp(name, "memory", 6) == 0) {
                in_memory_node = 1;
            }
            break;
        }

        case FDT_END_NODE:
            if (in_memory_node && depth == 1) {
                in_memory_node = 0;
            }
            depth--;
            break;

        case FDT_PROP: {
            uint32 len = fdt32_to_cpu(*(uint32 *)p);
            p += 4;
            uint32 nameoff = fdt32_to_cpu(*(uint32 *)p);
            p += 4;

            const char *propname = fdt_get_string(dtb, nameoff);
            void *data = p;
            p += fdt_align(len);

            // At root level, capture #address-cells and #size-cells
            if (depth == 1) {
                if (strcmp(propname, "#address-cells") == 0 && len >= 4) {
                    root_addr_cells = fdt32_to_cpu(*(uint32 *)data);
                } else if (strcmp(propname, "#size-cells") == 0 && len >= 4) {
                    root_size_cells = fdt32_to_cpu(*(uint32 *)data);
                }
            }

            // If we're in a memory node, look for "reg" property
            if (in_memory_node && strcmp(propname, "reg") == 0) {
                // Parse the first entry of the reg property
                uint32 *cells = (uint32 *)data;
                uint64 base = 0, size = 0;

                if (root_addr_cells == 2) {
                    base = ((uint64)fdt32_to_cpu(cells[0]) << 32) |
                           fdt32_to_cpu(cells[1]);
                } else {
                    base = fdt32_to_cpu(cells[0]);
                }

                int size_offset = root_addr_cells;
                if (root_size_cells == 2) {
                    size = ((uint64)fdt32_to_cpu(cells[size_offset]) << 32) |
                           fdt32_to_cpu(cells[size_offset + 1]);
                } else {
                    size = fdt32_to_cpu(cells[size_offset]);
                }

                *base_out = base;
                *size_out = size;
                return 0;  // Success!
            }
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            return -1;  // Reached end without finding memory

        default:
            return -1;  // Unknown token
        }
    }
}

// Parse and copy namestring into fdt_node
// The full namestring (including @addr) is stored for comparison
// name_size is the length of the base name (before @)
// The node->name points to the full string
static void fdt_parse_namestring(struct fdt_node *node, const char *namestring,
                                 bool copy, bool ignore_addr) {
    int len = 0;
    int full_len = strlen(namestring);

    // Find length of base name (up to '@' or null terminator)
    while (namestring[len] != '\0' && namestring[len] != '@') {
        len++;
    }

    node->name_size = len;
    node->hash = hlist_hash_str((char *)namestring, len);
    
    if (namestring[len] == '\0' || ignore_addr) {
        node->has_addr = 0;
        node->addr = 0;
        if (copy) {
            memcpy((char *)node->name, namestring, len + 1);
        } else {
            node->name = namestring;
        }
        return;
    }

    // Has unit address
    node->has_addr = 1;
    
    // Try to parse as hex number
    char *endptr = NULL;
    node->addr = strtoul(&namestring[len + 1], &endptr, 16);
    
    // If parsing failed or didn't consume all chars, hash the address string instead
    // This handles non-numeric unit addresses like "SPT_PD_VPU"
    if (endptr == &namestring[len + 1] || *endptr != '\0') {
        // Use hash of the unit address string as the numeric addr for comparison
        // Add a flag or use a different approach - for now, hash it
        node->addr = hlist_hash_str((char *)&namestring[len + 1], 
                                     full_len - len - 1);
    }
    
    return;
}

// fdt_node comparison order:
// - hash value of the name
// - string comparison of the name
// - nodes without unit address come first
// - nodes with unit address ordered by unit address value in ascending order
static int __fdt_rb_compare(uint64 a, uint64 b) {
    struct fdt_node *node_a = (struct fdt_node *)a;
    struct fdt_node *node_b = (struct fdt_node *)b;

    // hash value
    if (node_a->hash > node_b->hash)
        return 1;
    if (node_a->hash < node_b->hash)
        return -1;

    // string comparison
    int str_cmp = strcmp(node_a->name, node_b->name);
    if (str_cmp != 0)
        return str_cmp;

    // unit address presence
    if (node_a->has_addr == node_b->has_addr) {
        if (node_a->has_addr) {
            return (node_a->addr > node_b->addr)
                       ? 1
                       : ((node_a->addr < node_b->addr) ? -1 : 0);
        }
        return 0;
    }
    return node_a->has_addr ? 1 : -1;
}

static uint64 __fdt_rb_get_key(struct rb_node *node) {
    void *fdt_node = container_of(node, struct fdt_node, rb_entry);
    return (uint64)fdt_node;
}

static struct rb_root_opts fdt_rb_opts = {
    .keys_cmp_fun = __fdt_rb_compare,
    .get_key_fun = __fdt_rb_get_key,
};

static struct fdt_node *__fdt_create_node(size_t name_size, size_t data_size) {
    size_t total_size = sizeof(struct fdt_node) + data_size + name_size + 1;
    struct fdt_node *node =
        early_alloc_align(total_size, sizeof(uint64));
    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, total_size);
    node->data_size = data_size;
    node->name_size = name_size;
    node->name = (const char *)node->data + data_size;
    return node;
}

// Lookup a child node by name and optional unit address
// If addr is NULL, it will try to parse the unit address from the namestring
// If addr is provided, it will override any unit address in the namestring
struct fdt_node *fdt_node_lookup(struct fdt_node *parent,
                                 const char *name, uint64 *addr) {
    if (parent->child_count == 0) {
        return NULL;
    }
    struct fdt_node dummy = {};
    fdt_parse_namestring(&dummy, name, false, addr != NULL);
    // override addr if provided
    if (addr != NULL) {
        dummy.addr = *addr;
    }
    struct rb_node *node = rb_find_key(&parent->children, (uint64)&dummy);
    if (node == NULL) {
        return NULL;
    }
    return container_of(node, struct fdt_node, rb_entry);
}

// Find the first child node with the given name (ignoring unit address)
// Returns the first matching fdt_node, or NULL if not found
static struct fdt_node *__fdt_node_first(struct fdt_node *parent,
                                         const char *name) {
    if (parent->child_count == 0) {
        return NULL;
    }
    // Create a dummy node with no unit address to find the first match
    struct fdt_node dummy = {};
    fdt_parse_namestring(&dummy, name, false, true);  // ignore addr in name
    dummy.has_addr = 0;  // Look for nodes without unit address first

    // Use rb_find_key_rup to find the first node >= our dummy
    // Since nodes without unit address come before those with unit address,
    // and our dummy has has_addr=0 (smallest possible for this name),
    // rb_find_key_rup returns the first node with matching name (if any)
    struct rb_node *node = rb_find_key_rup(&parent->children, (uint64)&dummy);
    if (node == NULL) {
        return NULL;
    }
    struct fdt_node *fdt = container_of(node, struct fdt_node, rb_entry);
    // Verify the name matches (hash and string comparison)
    // Use strncmp with dummy.name_size since both names are the base name only
    if (fdt->hash != dummy.hash ||
        strncmp(fdt->name, dummy.name, dummy.name_size) != 0 ||
        fdt->name[dummy.name_size] != '\0') {
        return NULL;
    }
    return fdt;
}

// Find the next sibling node with the same name
// Returns the next matching fdt_node, or NULL if this is the last one
static struct fdt_node *__fdt_node_next_same_name(struct fdt_node *parent,
                                                   struct fdt_node *current) {
    if (parent == NULL || current == NULL) {
        return NULL;
    }
    // Get the next node in the tree
    struct rb_node *next = rb_next_node(&current->rb_entry);
    if (next == NULL) {
        return NULL;
    }
    struct fdt_node *next_fdt = container_of(next, struct fdt_node, rb_entry);
    // Check if the name matches (same hash and same name string)
    if (next_fdt->hash != current->hash ||
        strcmp(next_fdt->name, current->name) != 0) {
        return NULL;  // Reached the last node with this name
    }
    return next_fdt;
}

// Lookup a node by path (e.g., "/cpus/cpu@0" or "/soc/uart@10000000")
// Returns the matching fdt_node, or NULL if not found
// Path must start with '/' for absolute paths
struct fdt_node *fdt_path_lookup(struct fdt_blob_info *blob,
                                 const char *path) {
    if (blob == NULL || path == NULL || blob->root.node == NULL) {
        return NULL;
    }

    // Get the root node (the empty-named "/" node)
    struct fdt_node *current = container_of(blob->root.node, struct fdt_node, rb_entry);
    
    // Handle root path
    if (path[0] == '/' && path[1] == '\0') {
        return current;
    }

    // Skip leading '/'
    const char *p = path;
    if (*p == '/') {
        p++;
    }

    // Parse each path component
    while (*p != '\0') {
        // Find end of current component (next '/' or end of string)
        const char *end = p;
        while (*end != '\0' && *end != '/') {
            end++;
        }

        int comp_len = end - p;
        if (comp_len == 0) {
            // Empty component (e.g., "//"), skip it
            p = end + 1;
            continue;
        }

        // Check if component has unit address
        const char *at = p;
        while (at < end && *at != '@') {
            at++;
        }

        struct fdt_node *child = NULL;

        if (at < end) {
            // Has unit address - parse and use exact lookup
            int name_len = at - p;
            uint64 addr = strtoul(at + 1, NULL, 16);

            // Create dummy for lookup
            struct fdt_node dummy = {};
            dummy.name = p;
            dummy.name_size = name_len;
            dummy.hash = hlist_hash_str((char *)p, name_len);
            dummy.has_addr = 1;
            dummy.addr = addr;

            struct rb_node *node = rb_find_key(&current->children, (uint64)&dummy);
            if (node != NULL) {
                child = container_of(node, struct fdt_node, rb_entry);
            }
        } else {
            // No unit address - find first matching node
            // Create a temporary null-terminated copy of the component
            char name_buf[64];
            int copy_len = comp_len < 63 ? comp_len : 63;
            memcpy(name_buf, p, copy_len);
            name_buf[copy_len] = '\0';

            child = __fdt_node_first(current, name_buf);
        }

        if (child == NULL) {
            return NULL;  // Path component not found
        }

        current = child;

        // Move to next component
        if (*end == '/') {
            p = end + 1;
        } else {
            break;  // End of path
        }
    }

    return current;
}

// Insert a new node into the parent's children rb-tree
static bool __fdt_insert_node(struct fdt_blob_info *blob, struct fdt_node *parent,
                              struct fdt_node *new_node) {

    rb_node_init(&new_node->rb_entry);
    if (rb_insert_color(&parent->children, &new_node->rb_entry) !=
        &new_node->rb_entry) {
        return false;
    }

    parent->child_count++;
    blob->n_nodes++;
    list_node_push(&blob->all_nodes, new_node, list_entry);
    return true;
}

// Build an fdt_blob_info structure from a raw FDT blob
// Returns NULL on failure
static struct fdt_blob_info *fdt_build_blob_info(void *dtb) {
    if (!fdt_valid(dtb)) {
        printf("fdt: invalid magic\n");
        return NULL;
    }

    // Allocate the blob info structure
    struct fdt_blob_info *blob = early_alloc_align(sizeof(struct fdt_blob_info),
                                                    sizeof(uint64));
    if (blob == NULL) {
        printf("fdt: alloc blob failed\n");
        return NULL;
    }
    memset(blob, 0, sizeof(struct fdt_blob_info));

    // Copy original header
    memcpy(&blob->original_header, dtb, sizeof(struct fdt_header));
    blob->boot_cpuid_phys = fdt32_to_cpu(((struct fdt_header *)dtb)->boot_cpuid_phys);

    // Initialize the root rb-tree and all_nodes list
    rb_root_init(&blob->root, &fdt_rb_opts);
    list_entry_init(&blob->all_nodes);

    // Parse memory reservation map
    struct fdt_header *header = (struct fdt_header *)dtb;
    uint32 rsvmap_off = fdt32_to_cpu(header->off_mem_rsvmap);
    struct mem_region *rsvmap = (struct mem_region *)(dtb + rsvmap_off);
    struct mem_region *cur = rsvmap;

    int rsv_count = 0;
    while (cur->base != 0 || cur->size != 0) {
        rsv_count++;
        cur++;
    }

    if (rsv_count > 0) {
        blob->reserved = early_alloc_align(rsv_count * sizeof(struct mem_region),
                                           sizeof(uint64));
        if (blob->reserved == NULL) {
            printf("fdt: alloc reserved failed\n");
            return NULL;
        }
        blob->reserved_count = rsv_count;
        for (int i = 0; i < rsv_count; i++) {
            blob->reserved[i].base = fdt64_to_cpu(rsvmap[i].base);
            blob->reserved[i].size = fdt64_to_cpu(rsvmap[i].size);
        }
    }

    // Parse the FDT structure and build the tree
    char *struct_start = (char *)dtb + fdt_get_header(dtb, 8);  // off_dt_struct
    char *p = struct_start;
    uint32 token;

    // Stack for tracking parent nodes during tree construction
    #define FDT_MAX_DEPTH 32
    struct fdt_node *node_stack[FDT_MAX_DEPTH];
    int depth = 0;

    // Create a virtual root node to hold the actual root "/"
    struct fdt_node *virtual_root = __fdt_create_node(0, 0);
    if (virtual_root == NULL) {
        printf("fdt: alloc virtual root failed\n");
        return NULL;
    }
    rb_root_init(&virtual_root->children, &fdt_rb_opts);
    node_stack[0] = virtual_root;

    while (1) {
        token = fdt32_to_cpu(*(uint32 *)p);
        p += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = p;
            int namelen = strlen(name);
            p += fdt_align(namelen + 1);

            // Use dummy node to parse namestring and get length/addr info
            struct fdt_node dummy = {};
            fdt_parse_namestring(&dummy, name, false, false);

            // Create new node with space for name (no property data for nodes)
            struct fdt_node *new_node = __fdt_create_node(dummy.name_size, 0);
            if (new_node == NULL) {
                printf("fdt: alloc node '%s' failed\n", name);
                return NULL;
            }

            // Copy name string to the node's name buffer
            memcpy((char *)new_node->name, name, dummy.name_size);
            ((char *)new_node->name)[dummy.name_size] = '\0';

            // Copy parsed info
            new_node->hash = dummy.hash;
            new_node->has_addr = dummy.has_addr;
            new_node->addr = dummy.addr;
            new_node->layer = depth;

            // Initialize children rb-tree for this node
            rb_root_init(&new_node->children, &fdt_rb_opts);

            // Insert into parent's children
            struct fdt_node *parent = node_stack[depth];
            if (!__fdt_insert_node(blob, parent, new_node)) {
                printf("fdt: insert node '%s' failed (dup?)\n", name);
                return NULL;  // Insertion failed (duplicate?)
            }

            // Push this node onto stack as new parent
            depth++;
            if (depth >= FDT_MAX_DEPTH) {
                printf("fdt: tree too deep\n");
                return NULL;  // Tree too deep
            }
            node_stack[depth] = new_node;
            break;
        }

        case FDT_END_NODE:
            if (depth > 0) {
                depth--;
            }
            break;

        case FDT_PROP: {
            uint32 len = fdt32_to_cpu(*(uint32 *)p);
            p += 4;
            uint32 nameoff = fdt32_to_cpu(*(uint32 *)p);
            p += 4;

            const char *propname = fdt_get_string(dtb, nameoff);
            void *data = p;
            p += fdt_align(len);

            // Get current parent node
            struct fdt_node *parent = node_stack[depth];
            if (parent == NULL) {
                continue;
            }

            // Check for phandle property - set parent's phandle instead of creating node
            if (strcmp(propname, "phandle") == 0 ||
                strcmp(propname, "linux,phandle") == 0) {
                if (len >= 4) {
                    parent->phandle = fdt32_to_cpu(*(uint32 *)data);
                    parent->has_phandle = 1;
                }
                continue;
            }

            // Use dummy node to parse property name
            struct fdt_node dummy = {};
            fdt_parse_namestring(&dummy, propname, false, true);  // properties don't have @addr

            // Create new node with space for name and property data
            struct fdt_node *prop_node = __fdt_create_node(dummy.name_size, len);
            if (prop_node == NULL) {
                printf("fdt: alloc prop '%s' failed\n", propname);
                return NULL;
            }

            // Copy property data
            if (len > 0) {
                memcpy(prop_node->data, data, len);
            }

            // Copy property name
            memcpy((char *)prop_node->name, propname, dummy.name_size);
            ((char *)prop_node->name)[dummy.name_size] = '\0';

            // Copy parsed info
            prop_node->hash = dummy.hash;
            prop_node->has_addr = 0;  // Properties don't have unit addresses
            prop_node->layer = depth + 1;

            // Initialize children rb-tree (properties typically don't have children)
            rb_root_init(&prop_node->children, &fdt_rb_opts);

            // Insert into parent's children
            if (!__fdt_insert_node(blob, parent, prop_node)) {
                printf("fdt: insert prop '%s' in '%s' failed\n", propname, parent->name);
                return NULL;  // Insertion failed
            }
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            // Copy the root's children to blob->root
            blob->root = virtual_root->children;
            return blob;

        default:
            // Unknown token
            printf("fdt: unknown token 0x%x\n", token);
            return NULL;
        }
    }

    #undef FDT_MAX_DEPTH
}

// Helper to get a property node from a parent node
static struct fdt_node *__fdt_get_prop(struct fdt_node *node, const char *name) {
    return fdt_node_lookup(node, name, NULL);
}

// Helper to get property data as uint32
static uint32 __fdt_prop_u32(struct fdt_node *prop, int index) {
    if (prop == NULL || prop->data_size < (index + 1) * 4) {
        return 0;
    }
    return fdt32_to_cpu(prop->data[index]);
}

// Helper to get property data as uint64
static uint64 __fdt_prop_u64(struct fdt_node *prop, int index) {
    if (prop == NULL || prop->data_size < (index + 1) * 8) {
        return 0;
    }
    uint32 hi = fdt32_to_cpu(prop->data[index * 2]);
    uint32 lo = fdt32_to_cpu(prop->data[index * 2 + 1]);
    return ((uint64)hi << 32) | lo;
}

// Helper to check if property contains a compatible string
static bool __fdt_prop_compat(struct fdt_node *prop, const char *compat) {
    if (prop == NULL || prop->data_size == 0) {
        return false;
    }
    return strstr((const char *)prop->data, compat) != NULL;
}

// Parse reg property based on address-cells and size-cells
static void __fdt_parse_reg_prop(struct fdt_node *prop, int addr_cells, int size_cells,
                                  uint64 *base_out, uint64 *size_out) {
    if (prop == NULL || base_out == NULL) {
        return;
    }
    
    if (addr_cells == 2) {
        *base_out = __fdt_prop_u64(prop, 0);
    } else {
        *base_out = __fdt_prop_u32(prop, 0);
    }
    
    if (size_out != NULL) {
        if (size_cells == 2) {
            uint32 hi = __fdt_prop_u32(prop, addr_cells);
            uint32 lo = __fdt_prop_u32(prop, addr_cells + 1);
            *size_out = ((uint64)hi << 32) | lo;
        } else {
            *size_out = __fdt_prop_u32(prop, addr_cells);
        }
    }
}

// Global blob info
static struct fdt_blob_info *fdt_blob;

// Extract platform info from the built FDT tree
static void fdt_extract_platform_info(struct fdt_blob_info *blob) {
    // Get root node (the empty-named node "")
    struct fdt_node *root = __fdt_node_first(
        container_of(blob->root.node, struct fdt_node, rb_entry)->children.node ?
        container_of(blob->root.node, struct fdt_node, rb_entry) : NULL, "");
    
    // If root lookup failed, try to get the first node directly
    if (root == NULL && blob->root.node != NULL) {
        root = container_of(blob->root.node, struct fdt_node, rb_entry);
    }
    if (root == NULL) {
        return;
    }

    // Get root #address-cells and #size-cells (defaults: 2, 1)
    int root_addr_cells = 2, root_size_cells = 1;
    struct fdt_node *prop = __fdt_get_prop(root, "#address-cells");
    if (prop) root_addr_cells = __fdt_prop_u32(prop, 0);
    prop = __fdt_get_prop(root, "#size-cells");
    if (prop) root_size_cells = __fdt_prop_u32(prop, 0);

    // Parse /cpus node
    struct fdt_node *cpus = fdt_node_lookup(root, "cpus", NULL);
    if (cpus) {
        // Get timebase-frequency
        prop = __fdt_get_prop(cpus, "timebase-frequency");
        if (prop) {
            if (prop->data_size == 4) {
                platform.timebase_freq = __fdt_prop_u32(prop, 0);
            } else if (prop->data_size == 8) {
                platform.timebase_freq = __fdt_prop_u64(prop, 0);
            }
        }
        
        // Count CPU nodes
        struct fdt_node *cpu = __fdt_node_first(cpus, "cpu");
        while (cpu) {
            platform.ncpu++;
            cpu = __fdt_node_next_same_name(cpus, cpu);
        }
    }
    if (platform.ncpu == 0) platform.ncpu = 1;

    // Parse /memory node(s)
    struct fdt_node *memory = __fdt_node_first(root, "memory");
    while (memory && platform.mem_count < MAX_MEM_REGIONS) {
        prop = __fdt_get_prop(memory, "reg");
        if (prop) {
            int cells_per_entry = root_addr_cells + root_size_cells;
            int num_entries = (prop->data_size / 4) / cells_per_entry;
            
            for (int i = 0; i < num_entries && platform.mem_count < MAX_MEM_REGIONS; i++) {
                uint64 base = 0, size = 0;
                int offset = i * cells_per_entry;
                
                if (root_addr_cells == 2) {
                    base = ((uint64)__fdt_prop_u32(prop, offset) << 32) |
                           __fdt_prop_u32(prop, offset + 1);
                } else {
                    base = __fdt_prop_u32(prop, offset);
                }
                
                if (root_size_cells == 2) {
                    size = ((uint64)__fdt_prop_u32(prop, offset + root_addr_cells) << 32) |
                           __fdt_prop_u32(prop, offset + root_addr_cells + 1);
                } else {
                    size = __fdt_prop_u32(prop, offset + root_addr_cells);
                }
                
                platform.mem[platform.mem_count].base = base;
                platform.mem[platform.mem_count].size = size;
                platform.mem_count++;
                platform.total_mem += size;
            }
        }
        memory = __fdt_node_next_same_name(root, memory);
    }

    // Parse /chosen node for ramdisk
    struct fdt_node *chosen = fdt_node_lookup(root, "chosen", NULL);
    if (chosen) {
        prop = __fdt_get_prop(chosen, "linux,initrd-start");
        if (prop) {
            platform.ramdisk_base = (prop->data_size == 8) ?
                __fdt_prop_u64(prop, 0) : __fdt_prop_u32(prop, 0);
        }
        prop = __fdt_get_prop(chosen, "linux,initrd-end");
        if (prop) {
            uint64 end = (prop->data_size == 8) ?
                __fdt_prop_u64(prop, 0) : __fdt_prop_u32(prop, 0);
            if (platform.ramdisk_base != 0 && end > platform.ramdisk_base) {
                platform.ramdisk_size = end - platform.ramdisk_base;
                platform.has_ramdisk = 1;
            }
        }
    }

    // Parse /soc node for devices (common structure)
    struct fdt_node *soc = fdt_node_lookup(root, "soc", NULL);
    struct fdt_node *device_parent = soc ? soc : root;
    
    int soc_addr_cells = root_addr_cells, soc_size_cells = root_size_cells;
    if (soc) {
        prop = __fdt_get_prop(soc, "#address-cells");
        if (prop) soc_addr_cells = __fdt_prop_u32(prop, 0);
        prop = __fdt_get_prop(soc, "#size-cells");
        if (prop) soc_size_cells = __fdt_prop_u32(prop, 0);
    }

    // Scan all children for devices
    struct rb_node *rb = rb_first_node(&device_parent->children);
    while (rb) {
        struct fdt_node *node = container_of(rb, struct fdt_node, rb_entry);
        rb = rb_next_node(rb);
        
        // Skip properties (they don't have children typically, but check by name pattern)
        struct fdt_node *compat = __fdt_get_prop(node, "compatible");
        struct fdt_node *reg = __fdt_get_prop(node, "reg");
        struct fdt_node *interrupts = __fdt_get_prop(node, "interrupts");
        
        // UART detection
        if (platform.uart_base == 0 && compat &&
            (__fdt_prop_compat(compat, "ns16550") ||
             __fdt_prop_compat(compat, "uart") ||
             __fdt_prop_compat(compat, "serial"))) {
            if (reg) {
                __fdt_parse_reg_prop(reg, soc_addr_cells, soc_size_cells,
                                     &platform.uart_base, NULL);
            }
            if (interrupts) {
                platform.uart_irq = __fdt_prop_u32(interrupts, 0);
            }
        }
        
        // PLIC detection
        if (platform.plic_base == 0 && compat &&
            (__fdt_prop_compat(compat, "plic") ||
             __fdt_prop_compat(compat, "riscv,plic"))) {
            if (reg) {
                __fdt_parse_reg_prop(reg, soc_addr_cells, soc_size_cells,
                                     &platform.plic_base, &platform.plic_size);
            }
        }
        
        // VirtIO detection
        if (compat && __fdt_prop_compat(compat, "virtio") &&
            platform.virtio_count < 8) {
            platform.has_virtio = 1;
            if (reg) {
                __fdt_parse_reg_prop(reg, soc_addr_cells, soc_size_cells,
                                     &platform.virtio_base[platform.virtio_count], NULL);
            }
            if (interrupts) {
                platform.virtio_irq[platform.virtio_count] = __fdt_prop_u32(interrupts, 0);
            }
            platform.virtio_count++;
        }
    }

    // Copy reserved regions from blob (from memreserve block)
    platform.reserved = blob->reserved;
    platform.reserved_count = blob->reserved_count;

    // Also parse /reserved-memory node for additional reserved regions
    struct fdt_node *rsvmem = fdt_node_lookup(root, "reserved-memory", NULL);
    if (rsvmem) {
        // Get address-cells and size-cells for this node
        int rsv_addr_cells = root_addr_cells, rsv_size_cells = root_size_cells;
        prop = __fdt_get_prop(rsvmem, "#address-cells");
        if (prop) rsv_addr_cells = __fdt_prop_u32(prop, 0);
        prop = __fdt_get_prop(rsvmem, "#size-cells");
        if (prop) rsv_size_cells = __fdt_prop_u32(prop, 0);

        // Count reserved memory child nodes first
        int rsv_child_count = 0;
        struct rb_node *rsv_rb = rb_first_node(&rsvmem->children);
        while (rsv_rb) {
            struct fdt_node *rsv_node = container_of(rsv_rb, struct fdt_node, rb_entry);
            rsv_rb = rb_next_node(rsv_rb);
            // Only count nodes with reg property (skip properties and ranges)
            if (__fdt_get_prop(rsv_node, "reg") != NULL) {
                rsv_child_count++;
            }
        }

        // Allocate combined array if we have new reserved regions
        if (rsv_child_count > 0) {
            int total_count = platform.reserved_count + rsv_child_count;
            struct mem_region *new_reserved = early_alloc_align(
                total_count * sizeof(struct mem_region), sizeof(uint64));
            if (new_reserved != NULL) {
                // Copy existing reserved regions
                for (int i = 0; i < platform.reserved_count; i++) {
                    new_reserved[i] = platform.reserved[i];
                }
                // Parse and add new reserved regions
                int idx = platform.reserved_count;
                rsv_rb = rb_first_node(&rsvmem->children);
                while (rsv_rb && idx < total_count) {
                    struct fdt_node *rsv_node = container_of(rsv_rb, struct fdt_node, rb_entry);
                    rsv_rb = rb_next_node(rsv_rb);
                    
                    prop = __fdt_get_prop(rsv_node, "reg");
                    if (prop) {
                        uint64 base = 0, size = 0;
                        if (rsv_addr_cells == 2) {
                            base = ((uint64)__fdt_prop_u32(prop, 0) << 32) |
                                   __fdt_prop_u32(prop, 1);
                        } else {
                            base = __fdt_prop_u32(prop, 0);
                        }
                        if (rsv_size_cells == 2) {
                            size = ((uint64)__fdt_prop_u32(prop, rsv_addr_cells) << 32) |
                                   __fdt_prop_u32(prop, rsv_addr_cells + 1);
                        } else {
                            size = __fdt_prop_u32(prop, rsv_addr_cells);
                        }
                        new_reserved[idx].base = base;
                        new_reserved[idx].size = size;
                        idx++;
                    }
                }
                platform.reserved = new_reserved;
                platform.reserved_count = idx;
            }
        }
    }
}

int fdt_init(void *dtb) {
    printf("fdt: checking DTB at %p\n", dtb);

    if (!fdt_valid(dtb)) {
        printf("fdt: no valid DTB found!\n");
        return -1;
    }

    printf("fdt: using DTB at %p (size %d bytes)\n", dtb, fdt_totalsize(dtb));

    // Initialize platform info with zeros
    memset(&platform, 0, sizeof(platform));

    // Build the FDT tree structure
    fdt_blob = fdt_build_blob_info(dtb);
    if (fdt_blob == NULL) {
        printf("fdt: failed to build blob info!\n");
        return -1;
    }

    printf("fdt: parsed %d nodes\n", fdt_blob->n_nodes);

    // Extract platform info from the tree
    fdt_extract_platform_info(fdt_blob);

    // Print discovered hardware
    printf("fdt: probed platform info:\n");
    printf("  Memory regions: %d (total %ld MB)\n", platform.mem_count,
           platform.total_mem / (1024 * 1024));
    for (int i = 0; i < platform.mem_count; i++) {
        printf("    [%d] 0x%lx - 0x%lx (%ld MB)\n", i, platform.mem[i].base,
               platform.mem[i].base + platform.mem[i].size,
               platform.mem[i].size / (1024 * 1024));
    }
    printf("  Reserved regions: %d\n", platform.reserved_count);
    for (int i = 0; i < platform.reserved_count; i++) {
        printf("    [%d] 0x%lx - 0x%lx (%ld KB)\n", i,
               platform.reserved[i].base,
               platform.reserved[i].base + platform.reserved[i].size,
               platform.reserved[i].size / 1024);
    }
    if (platform.has_ramdisk) {
        printf("  Ramdisk: 0x%lx - 0x%lx (%ld KB)\n", platform.ramdisk_base,
               platform.ramdisk_base + platform.ramdisk_size,
               platform.ramdisk_size / 1024);
    }
    printf("  UART: 0x%lx, IRQ %d\n", platform.uart_base, platform.uart_irq);
    printf("  PLIC: 0x%lx (size 0x%lx)\n", platform.plic_base, platform.plic_size);
    printf("  CPUs: %d, timebase: %ld Hz\n", platform.ncpu, platform.timebase_freq);

    if (platform.has_virtio) {
        printf("  VirtIO devices: %d\n", platform.virtio_count);
        for (int i = 0; i < platform.virtio_count; i++) {
            printf("    [%d] 0x%lx, IRQ %d\n", i, platform.virtio_base[i],
                   platform.virtio_irq[i]);
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

// Print indentation for tree structure
static void fdt_print_indent(int depth) {
    for (int i = 0; i < depth; i++)
        printf("  ");
}

// Print a property value in a readable format
static void fdt_print_prop_value(const char *name, void *data, uint32 len) {
    // Empty property
    if (len == 0) {
        printf("(empty)\n");
        return;
    }

    // Check if it looks like a string (printable, null-terminated)
    char *str = (char *)data;
    int is_string = 1;
    int null_count = 0;
    for (uint32 i = 0; i < len; i++) {
        if (str[i] == '\0') {
            null_count++;
        } else if (str[i] < 0x20 || str[i] > 0x7e) {
            is_string = 0;
            break;
        }
    }

    // String or string list - print each null-terminated segment
    if (is_string && null_count > 0 && str[len - 1] == '\0') {
        char *s = str;
        char *end = str + len;
        int first = 1;
        while (s < end) {
            if (*s != '\0') {
                if (!first)
                    printf(", ");
                printf("\"%s\"", s);
                first = 0;
                // Skip to next null
                while (s < end && *s != '\0')
                    s++;
            }
            s++;
        }
        printf("\n");
        return;
    }

    // 32-bit cells (most common for reg, interrupts, etc.)
    if (len % 4 == 0) {
        printf("<");
        uint32 *cells = (uint32 *)data;
        int ncells = len / 4;
        for (int i = 0; i < ncells; i++) {
            printf("0x%x", fdt32_to_cpu(cells[i]));
            if (i < ncells - 1)
                printf(" ");
        }
        printf(">\n");
        return;
    }

    // Raw bytes
    printf("[");
    uint8 *bytes = (uint8 *)data;
    for (uint32 i = 0; i < len && i < 32; i++) {
        printf("%02x", bytes[i]);
        if (i < len - 1)
            printf(" ");
    }
    if (len > 32)
        printf(" ...");
    printf("]\n");
}

// Recursive helper to walk a node and its children
static void __fdt_walk_node(struct fdt_node *node, int depth) {
    if (node == NULL) {
        return;
    }

    fdt_print_indent(depth);

    // Check if this is a property (has data) or a node (has children or is a container)
    if (node->data_size > 0) {
        // Property node - print name = value
        printf("%s = ", node->name);
        fdt_print_prop_value(node->name, node->data, node->data_size);
    } else {
        // Container node - print name with trailing /
        if (node->name[0] == '\0') {
            printf("/\n");
        } else if (node->has_addr) {
            printf("%s@%lx/\n", node->name, node->addr);
        } else {
            printf("%s/\n", node->name);
        }

        // Print phandle if present (stored on node, not as child property)
        if (node->has_phandle) {
            fdt_print_indent(depth + 1);
            printf("phandle = <0x%x>\n", node->phandle);
        }

        // Recursively walk children
        struct rb_node *rb = rb_first_node(&node->children);
        while (rb) {
            struct fdt_node *child = container_of(rb, struct fdt_node, rb_entry);
            __fdt_walk_node(child, depth + 1);
            rb = rb_next_node(rb);
        }
    }
}

void fdt_walk(void *dtb) {
    (void)dtb;  // Unused - we walk the parsed tree instead

    if (fdt_blob == NULL) {
        printf("fdt_walk: no parsed FDT tree available\n");
        return;
    }

    printf("=== FDT Walk (from parsed tree) ===\n");
    printf("Parsed %d nodes\n\n", fdt_blob->n_nodes);

    // Get the root node and walk it
    if (fdt_blob->root.node != NULL) {
        struct fdt_node *root = container_of(fdt_blob->root.node, 
                                              struct fdt_node, rb_entry);
        __fdt_walk_node(root, 0);
    }

    printf("\n=== End of FDT ===\n");
}
