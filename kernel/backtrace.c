#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "defs.h"
#include "printf.h"
#include "bintree.h"
#include "rbtree.h"

extern char stack0[];

// New format (optimized - adjacent symbols share boundaries):
// <file name>:
// :<symbol>
// <start address> <line number>
// ...
// :/
// <end address> 0
//
// The ':/' guard marks end of each file's symbols (line=0).
// Lookup finds entry with largest start_addr <= target.
typedef struct {
    struct rb_node rb;      // rb-tree node, keyed by start_addr
    void *start_addr;
    uint32 line;
    const char *symbol;     // Points to symbol name (after ':')
    uint16 symbol_len;
    const char *filename;   // Points to filename (before ':')
    uint16 filename_len;
} __ksymbols_t;

// rb-tree ops for symbol lookup
// Two-level key: start_addr first, then node address as tie-breaker
// This allows multiple entries with the same start_addr
static int ksym_keys_cmp(uint64 a, uint64 b) {
    __ksymbols_t *sym_a = (__ksymbols_t *)a;
    __ksymbols_t *sym_b = (__ksymbols_t *)b;
    
    // Primary key: start_addr
    if ((uint64)sym_a->start_addr < (uint64)sym_b->start_addr) return -1;
    if ((uint64)sym_a->start_addr > (uint64)sym_b->start_addr) return 1;
    
    // Secondary key: node address (tie-breaker for duplicates)
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static uint64 ksym_get_key(struct rb_node *node) {
    __ksymbols_t *sym = container_of(node, __ksymbols_t, rb);
    return (uint64)sym;  // Return pointer to the symbol struct as key
}

static struct rb_root_opts __ksym_rb_opts = {
    .keys_cmp_fun = ksym_keys_cmp,
    .get_key_fun = ksym_get_key,
};

// Special comparison for "round down" search - finds first node with start_addr >= target
// When keys are equal, return 1 to continue searching left (find minimum)
static int ksym_keys_cmp_rdown(uint64 a, uint64 b) {
    __ksymbols_t *sym_a = (__ksymbols_t *)a;
    __ksymbols_t *sym_b = (__ksymbols_t *)b;
    
    if ((uint64)sym_a->start_addr < (uint64)sym_b->start_addr) return -1;
    if ((uint64)sym_a->start_addr > (uint64)sym_b->start_addr) return 1;
    // Equal start_addr: return 1 to find the first (minimum) entry
    if (a == 0) return 0;  // Dummy node comparison
    return 1;
}

static struct rb_root_opts __ksym_rb_rdown_opts = {
    .keys_cmp_fun = ksym_keys_cmp_rdown,
    .get_key_fun = ksym_get_key,
};

static struct rb_root __ksym_rb_root = {
    .node = NULL,
    .opts = &__ksym_rb_opts,
};

static __ksymbols_t *__ksymbols;    // Storage pool for symbol entries

static int __ksymbol_count = -1;

void
ksymbols_init(void)
{
    __ksymbols = (void *)KERNEL_SYMBOLS_IDX_START;
    __ksymbol_count = 0;

    // Check if symbols are embedded
    if (KERNEL_SYMBOLS_SIZE == 0 || KERNEL_SYMBOLS_START == KERNEL_SYMBOLS_END) {
        printf("ksymbols: no embedded symbols found\n");
        return;
    }

    printf("ksymbols: loading embedded symbols from 0x%lx-0x%lx (%ld bytes)\n",
           KERNEL_SYMBOLS_START, KERNEL_SYMBOLS_END, KERNEL_SYMBOLS_SIZE);

    const char *current_file = NULL;
    int current_file_len = 0;
    const char *current_symbol = NULL;
    int current_symbol_len = 0;
    
    const char *line_start = (const char *)KERNEL_SYMBOLS_START;
    const char *end = (const char *)KERNEL_SYMBOLS_END;
    
    for (const char *p = line_start; p < end; p++) {
        if (*p == '\n' || *p == '\0') {
            const char *line_end = p;
            int line_len = line_end - line_start;
            
            if (line_len == 0) {
                // Empty line, skip
            } else if (line_start[line_len - 1] == ':' && line_start[0] != ':') {
                // File header: "filename:"
                current_file = line_start;
                current_file_len = line_len - 1;  // Exclude ':'
            } else if (line_start[0] == ':') {
                // Symbol header: ":symbol"
                current_symbol = line_start + 1;  // Skip ':'
                current_symbol_len = line_len - 1;
            } else {
                // Address line: "start_addr line_number"
                // Parse start address
                char *next = NULL;
                uint64 start_addr = strtoul(line_start, &next, 16);
                if (next && *next == ' ') {
                    next++;
                    uint32 line_num = (uint32)strtoul(next, NULL, 10);
                    
                    // Store the entry
                    if (__ksymbol_count < (int)(KERNEL_SYMBOLS_IDX_SIZE / sizeof(__ksymbols_t))) {
                        __ksymbols_t *entry = &__ksymbols[__ksymbol_count];
                        entry->start_addr = (void *)start_addr;
                        entry->line = line_num;
                        entry->symbol = current_symbol;
                        entry->symbol_len = current_symbol_len;
                        entry->filename = current_file;
                        entry->filename_len = current_file_len;
                        
                        // Initialize rb node and insert into tree
                        rb_node_init(&entry->rb);
                        rb_insert_color(&__ksym_rb_root, &entry->rb);
                        
                        __ksymbol_count++;
                    }
                }
            }
            
            line_start = p + 1;
            
            if (*p == '\0') {
                break;
            }
        }
    }

    printf("Kernel symbols initialized: %d entries\n", __ksymbol_count);
}

// Search for symbol containing the given address using rb-tree
// Returns pointer to __ksymbols_t or NULL if not found
// Finds entry with largest start_addr <= addr
// Guard entries (symbol='/') have line=0 and are skipped
static __ksymbols_t *
bt_search_sym(uint64 addr)
{
    if (__ksymbol_count <= 0) {
        return NULL;
    }

    // Create a dummy symbol for searching with the target address
    __ksymbols_t dummy = { .start_addr = (void *)addr };
    
    // Use rdown opts to find the last entry with start_addr <= addr
    struct rb_root search_root = __ksym_rb_root;
    search_root.opts = &__ksym_rb_rdown_opts;
    
    struct rb_node *node = rb_find_key_rdown(&search_root, (uint64)&dummy);
    if (node == NULL) {
        return NULL;
    }
    
    __ksymbols_t *sym = container_of(node, __ksymbols_t, rb);
    
    // Skip guard entries (symbol='/' has line=0)
    if (sym->line == 0) {
        return NULL;
    }
    
    return sym;
}

int
bt_search(uint64 addr, char *buf, size_t buflen, void **return_addr)
{
    __ksymbols_t *sym = bt_search_sym(addr);
    if (sym == NULL) {
        buf[0] = '\0';
        return -1;
    }
    
    if (sym->symbol && sym->symbol_len > 0) {
        size_t copy_len = sym->symbol_len;
        if (copy_len >= buflen) {
            copy_len = buflen - 1;
        }
        memmove(buf, sym->symbol, copy_len);
        buf[copy_len] = '\0';
    } else {
        buf[0] = '\0';
    }
    
    if (return_addr) {
        *return_addr = sym->start_addr;
    }
    
    // Return index for backwards compatibility (though not strictly needed)
    return (int)(sym - __ksymbols);
}

// Get file:line info for a symbol pointer
static void
bt_get_location_sym(__ksymbols_t *sym, char *filebuf, size_t filebuflen, uint32 *line)
{
    if (sym == NULL) {
        filebuf[0] = '\0';
        *line = 0;
        return;
    }
    
    if (sym->filename && sym->filename_len > 0) {
        size_t copy_len = sym->filename_len;
        if (copy_len >= filebuflen) {
            copy_len = filebuflen - 1;
        }
        memmove(filebuf, sym->filename, copy_len);
        filebuf[copy_len] = '\0';
    } else {
        filebuf[0] = '\0';
    }
    
    *line = sym->line;
}

// Get offset from symbol start for a given address
static int
bt_get_offset_sym(__ksymbols_t *sym, uint64 addr)
{
    if (sym == NULL) {
        return 0;
    }
    return (int)((void *)addr - sym->start_addr);
}

#define BT_FRAME_TOP(__fp)          ((__fp) ? *(uint64 *)((uint64)(__fp) - 16) : 0)
#define BT_RETURN_ADDRESS(__fp)     ((__fp) ? *(uint64 *)((uint64)(__fp) - 8) : 0)
#define BT_IS_TOP_FRAME(__fp)       (!(uint64)(__fp) || (uint64)(__fp) == PGROUNDDOWN((uint64)(__fp)))

void
print_backtrace(uint64 context, uint64 stack_start, uint64 stack_end)
{
    printf("backtrace:\n");
    uint64 last_fp = context;
    for (uint64 fp = BT_FRAME_TOP(context), depth = 0;
         !BT_IS_TOP_FRAME(fp) && depth < BACKTRACE_MAX_DEPTH;
         last_fp = fp, fp = BT_FRAME_TOP(fp), depth++) {

        if (fp < stack_start || fp >= stack_end) {
            printf("  * unknown frame: %p\n", (void *)fp);
            break;
        } else if (fp == 0) {
            printf("  top frame\n");
            break;
        }

        char symbuf[64] = { 0 };
        char filebuf[128] = { 0 };
        uint32 line = 0;
        void *sym_addr = NULL;
        uint64 return_addr_val = BT_RETURN_ADDRESS(last_fp);
        if (return_addr_val == 0) {
            printf("  top frame\n");
            break;
        }
        int idx = bt_search(return_addr_val, symbuf, sizeof(symbuf), &sym_addr);
        if (idx < 0) {
            printf("  * %p: unknown\n", (void *)return_addr_val);
        } else {
            __ksymbols_t *sym = bt_search_sym(return_addr_val);
            bt_get_location_sym(sym, filebuf, sizeof(filebuf), &line);
            int offset = bt_get_offset_sym(sym, return_addr_val);
            printf("  * %s:%d: %s+%d\n", filebuf, line, symbuf, offset);
        }
    }
}

// Backtrace a process using its saved context.
// The process must be in a sleeping/blocked state (not running on any CPU).
// @param context: pointer to the process's saved context (struct context)
// @param kstack: base address of kernel stack
// @param kstack_order: order of kernel stack size (size = 1 << (PAGE_SHIFT + order))
void
print_proc_backtrace(struct context *ctx, uint64 kstack, int kstack_order)
{
    if (ctx == NULL || kstack == 0) {
        printf("backtrace: invalid context or stack\n");
        return;
    }
    
    // s0 is the frame pointer in RISC-V
    uint64 fp = ctx->s0;
    uint64 stack_size = (1UL << (PAGE_SHIFT + kstack_order));
    uint64 stack_start = kstack;
    uint64 stack_end = kstack + stack_size;
    
    printf("backtrace:\n");
    
    // First, print the return address from context (where the process will resume)
    char symbuf[64] = { 0 };
    char filebuf[128] = { 0 };
    uint32 line = 0;
    __ksymbols_t *sym = bt_search_sym(ctx->ra);
    if (sym == NULL) {
        printf("  > %p: unknown (resume point)\n", (void *)ctx->ra);
    } else {
        if (sym->symbol && sym->symbol_len > 0) {
            size_t copy_len = sym->symbol_len < sizeof(symbuf) - 1 ? sym->symbol_len : sizeof(symbuf) - 1;
            memmove(symbuf, sym->symbol, copy_len);
            symbuf[copy_len] = '\0';
        }
        bt_get_location_sym(sym, filebuf, sizeof(filebuf), &line);
        int offset = bt_get_offset_sym(sym, ctx->ra);
        printf("  > %s:%d: %s+%d (resume point) [%p]\n", filebuf, line, symbuf, offset, (void *)ctx->ra);
    }
    
    // Now walk the stack frames
    uint64 last_fp = fp;
    uint64 last_return_addr = ctx->ra;  // Track last return address to detect loops
    int repeat_count = 0;
    const int MAX_REPEATS = 3;
    
    for (uint64 curr_fp = BT_FRAME_TOP(fp), depth = 0;
         !BT_IS_TOP_FRAME(curr_fp) && depth < BACKTRACE_MAX_DEPTH;
         last_fp = curr_fp, curr_fp = BT_FRAME_TOP(curr_fp), depth++) {

        if (curr_fp < stack_start || curr_fp >= stack_end) {
            printf("  * frame outside stack: %p\n", (void *)curr_fp);
            break;
        }

        uint64 return_addr_val = BT_RETURN_ADDRESS(last_fp);
        if (return_addr_val == 0) {
            break;
        }
        
        // Detect repeated entries (same return address)
        if (return_addr_val == last_return_addr) {
            repeat_count++;
            if (repeat_count >= MAX_REPEATS) {
                printf("  * ... (%ld more repeated frames)\n", depth);
                break;
            }
        } else {
            repeat_count = 0;
            last_return_addr = return_addr_val;
        }
        
        sym = bt_search_sym(return_addr_val);
        if (sym == NULL) {
            printf("  * %p: unknown\n", (void *)return_addr_val);
        } else {
            if (sym->symbol && sym->symbol_len > 0) {
                size_t copy_len = sym->symbol_len < sizeof(symbuf) - 1 ? sym->symbol_len : sizeof(symbuf) - 1;
                memmove(symbuf, sym->symbol, copy_len);
                symbuf[copy_len] = '\0';
            } else {
                symbuf[0] = '\0';
            }
            bt_get_location_sym(sym, filebuf, sizeof(filebuf), &line);
            int offset = bt_get_offset_sym(sym, return_addr_val);
            printf("  * %s:%d: %s+%d [%p]\n", filebuf, line, symbuf, offset, (void *)return_addr_val);
        }
    }
}

// set breakpoint for GDB
void db_break(void) {
    return;
}

