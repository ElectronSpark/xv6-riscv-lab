/**
 * @file platform_x86.c
 * @brief x86_64 platform parser implementation.
 *
 * Parses PVH start_info or multiboot memory maps, populates the shared
 * platform_info structure, and provides early debugcon diagnostics.
 */

#include "types.h"
#include "compiler.h"
#include "platform.h"
#include "printf.h"
#include "string.h"
#include "dev/fdt.h"
#include "dev/uart.h"
#include "proc/sched.h"
#include "lock/rcu.h"
#include "lapic.h"
#include "x86.h"
#include "param.h"
#include <mm/memlayout.h>
#include "mm/page.h"
#include <mm/vm.h>
#include <mm/early_allocator.h>
#include "smp/percpu.h"

/* Global platform info structure (RISC-V defines this in fdt.c) */
struct platform_info platform;

static int x86_boot_cpu_limit_cached;
static void **x86_kernel_stacks;
static uint64 *x86_ap_boot_stack_tops;

extern void arch_relocate_stack_finish(uint64 delta);

/* ------------------------------------------------------------------ */
/*  Debugcon helpers (port 0xE9 — QEMU ISA debug console)             */
/* ------------------------------------------------------------------ */

static inline void x86_debug_outb(uint16 port, uint8 value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void x86_debug_puts(const char *s)
{
    while (*s)
        x86_debug_outb(0xE9, (uint8)*s++);
}

static void x86_debug_put_hex_compact(uint64 value)
{
    static const char hex[] = "0123456789abcdef";
    if (value == 0) {
        x86_debug_outb(0xE9, '0');
        return;
    }
    int started = 0;
    for (int i = 15; i >= 0; i--) {
        int nibble = (value >> (i * 4)) & 0xF;
        if (nibble || started) {
            x86_debug_outb(0xE9, (uint8)hex[nibble]);
            started = 1;
        }
    }
}

static void x86_debug_put_dec_u64(uint64 value)
{
    char buf[21];
    int idx = 20;
    buf[idx] = '\0';
    if (value == 0) {
        x86_debug_outb(0xE9, '0');
        return;
    }
    while (value > 0 && idx > 0) {
        idx--;
        buf[idx] = '0' + (value % 10);
        value /= 10;
    }
    x86_debug_puts(&buf[idx]);
}

static void x86_debug_put_size(uint64 size)
{
    if (size >= (1ULL << 30) && (size & ((1ULL << 30) - 1)) == 0) {
        x86_debug_put_dec_u64(size >> 30);
        x86_debug_puts(" GB");
    } else if (size >= (1ULL << 20) && (size & ((1ULL << 20) - 1)) == 0) {
        x86_debug_put_dec_u64(size >> 20);
        x86_debug_puts(" MB");
    } else if (size >= (1ULL << 20)) {
        x86_debug_put_dec_u64(size >> 20);
        x86_debug_puts(" MB + ");
        uint64 remainder = size - ((size >> 20) << 20);
        if (remainder >= (1ULL << 10)) {
            x86_debug_put_dec_u64(remainder >> 10);
            x86_debug_puts(" KB");
        } else {
            x86_debug_put_dec_u64(remainder);
            x86_debug_puts(" B");
        }
    } else if (size >= (1ULL << 10) && (size & ((1ULL << 10) - 1)) == 0) {
        x86_debug_put_dec_u64(size >> 10);
        x86_debug_puts(" KB");
    } else if (size >= (1ULL << 10)) {
        x86_debug_put_dec_u64(size >> 10);
        x86_debug_puts(" KB + ");
        x86_debug_put_dec_u64(size & 0x3FF);
        x86_debug_puts(" B");
    } else {
        x86_debug_put_dec_u64(size);
        x86_debug_puts(" B");
    }
}

static void x86_cpuid(uint32 leaf, uint32 subleaf, uint32 *a, uint32 *b,
                      uint32 *c, uint32 *d)
{
    asm volatile("cpuid"
                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(leaf), "c"(subleaf));
}

static uint32 x86_cpuid_topology_count(uint32 leaf)
{
    uint32 count = 0;

    for (uint32 subleaf = 0; subleaf < 8; subleaf++) {
        uint32 a, b, c, d;
        x86_cpuid(leaf, subleaf, &a, &b, &c, &d);
        uint32 level_type = (c >> 8) & 0xff;
        uint32 logical = b & 0xffff;
        if (level_type == 0)
            break;
        if (logical > count)
            count = logical;
    }

    return count;
}

static int x86_detect_boot_cpu_limit(void)
{
    uint32 a, b, c, d;
    uint32 count = 0;

    x86_cpuid(0, 0, &a, &b, &c, &d);
    uint32 max_leaf = a;

    if (max_leaf >= 0x1f)
        count = x86_cpuid_topology_count(0x1f);
    if (count == 0 && max_leaf >= 0x0b)
        count = x86_cpuid_topology_count(0x0b);
    if (count == 0 && max_leaf >= 1) {
        x86_cpuid(1, 0, &a, &b, &c, &d);
        count = (b >> 16) & 0xff;
    }
    if (count == 0)
        count = 1;

    if (count > MAX_CPUS) {
        printf("[SMP] CPUID reports %d CPUs; clamping to MAX_CPUS=%d\n",
               count, MAX_CPUS);
        count = MAX_CPUS;
    }

    return (int)count;
}

static uint64 x86_kernel_stack_top(int cpu)
{
    if (cpu < 0 || cpu >= platform_boot_cpu_limit())
        panic("x86_kernel_stack_top: invalid CPU %d", cpu);

    if (x86_kernel_stacks == NULL) {
        x86_kernel_stacks = page_alloc(0, PAGE_TYPE_ANON);
        if (x86_kernel_stacks == NULL)
            panic("x86_kernel_stack_top: stack table allocation failed");
        memset(x86_kernel_stacks, 0, PAGE_SIZE);
    }

    if (x86_kernel_stacks[cpu] == NULL) {
        void *stack = page_alloc(KERNEL_STACK_ORDER, PAGE_TYPE_ANON);
        if (stack == NULL)
            panic("x86_kernel_stack_top: kernel stack allocation failed");
        memset(stack, 0, KERNEL_STACK_SIZE);
        x86_kernel_stacks[cpu] = stack;
    }

    return (uint64)x86_kernel_stacks[cpu] + KERNEL_STACK_SIZE;
}

static void x86_relocate_current_stack(uint64 new_stack_top)
{
    uint64 old_sp = r_sp();
    uint64 old_fp = r_fp();
    uint64 old_base = old_sp & ~(KERNEL_STACK_SIZE - 1);
    uint64 old_limit = old_base + KERNEL_STACK_SIZE - PAGE_SIZE;
    uint64 new_base = new_stack_top - KERNEL_STACK_SIZE;
    uint64 delta = new_base - old_base;

    if (old_sp < old_base || old_sp >= old_limit)
        panic("x86_relocate_current_stack: SP outside current kernel stack");

    uint64 bytes = old_limit - old_sp;
    memmove((void *)(old_sp + delta), (void *)old_sp, bytes);

    for (uint64 fp = old_fp; fp >= old_sp && fp + sizeof(uint64) <= old_limit;) {
        uint64 *new_fp = (uint64 *)(fp + delta);
        uint64 next = *new_fp;
        if (next < old_sp || next >= old_limit)
            break;
        *new_fp = next + delta;
        if (next <= fp)
            break;
        fp = next;
    }

    arch_relocate_stack_finish(delta);
}

static void x86_debug_boot_mem_summary(void)
{
    x86_debug_puts("[xv6 x86_64] bootmem reg=");
    x86_debug_put_dec_u64((uint64)platform.mem_count);
    x86_debug_puts(" res=");
    x86_debug_put_dec_u64((uint64)platform.reserved_count);
    x86_debug_puts(" total=");
    x86_debug_put_size(platform.total_mem);
    x86_debug_puts("\n");

    for (int i = 0; i < platform.mem_count; i++) {
        uint64 base = platform.mem[i].base;
        uint64 end  = platform.mem[i].base + platform.mem[i].size;
        x86_debug_puts("[xv6 x86_64] mem[");
        x86_debug_put_dec_u64((uint64)i);
        x86_debug_puts("] [0x");
        x86_debug_put_hex_compact(base);
        x86_debug_puts("-0x");
        x86_debug_put_hex_compact(end);
        x86_debug_puts(") ");
        x86_debug_put_size(platform.mem[i].size);
        x86_debug_puts("\n");
    }

    for (int i = 0; i < platform.reserved_count; i++) {
        uint64 base = platform.reserved[i].base;
        uint64 end  = platform.reserved[i].base + platform.reserved[i].size;
        x86_debug_puts("[xv6 x86_64] res[");
        x86_debug_put_dec_u64((uint64)i);
        x86_debug_puts("] [0x");
        x86_debug_put_hex_compact(base);
        x86_debug_puts("-0x");
        x86_debug_put_hex_compact(end);
        x86_debug_puts(") ");
        x86_debug_put_size(platform.reserved[i].size);
        x86_debug_puts("\n");
    }

    if (platform.has_ramdisk) {
        x86_debug_puts("[xv6 x86_64] ramdisk [0x");
        x86_debug_put_hex_compact(platform.ramdisk_base);
        x86_debug_puts("-0x");
        x86_debug_put_hex_compact(platform.ramdisk_base + platform.ramdisk_size);
        x86_debug_puts(") ");
        x86_debug_put_size(platform.ramdisk_size);
        x86_debug_puts("\n");
    }
}

static void x86_print_region_list(const char *tag, struct mem_region *regions,
                                  int count)
{
    for (int i = 0; i < count; i++) {
        uint64 base = regions[i].base;
        uint64 end = base + regions[i].size;
        printf("x86 %s[%d]: [0x%lx-0x%lx) size=0x%lx\n", tag, i, base, end,
               regions[i].size);
    }
}

static void x86_print_kvm_region(const char *tag, uint64 base, uint64 end)
{
    printf("x86 %s: [0x%lx-0x%lx) size=0x%lx\n", tag, base, end,
           end - base);
}

/* ------------------------------------------------------------------ */
/*  Bootloader structure definitions                                  */
/* ------------------------------------------------------------------ */

struct x86_multiboot_info {
    uint32 flags;
    uint32 mem_lower;
    uint32 mem_upper;
    uint32 boot_device;
    uint32 cmdline;
    uint32 mods_count;
    uint32 mods_addr;
    uint32 syms[4];
    uint32 mmap_length;
    uint32 mmap_addr;
} __PACKED;

struct x86_multiboot_mmap_entry {
    uint32 size;
    uint64 addr;
    uint64 len;
    uint32 type;
} __PACKED;

struct x86_multiboot_module {
    uint32 mod_start;
    uint32 mod_end;
    uint32 string;
    uint32 reserved;
} __PACKED;

struct x86_pvh_start_info {
    uint32 magic;
    uint32 version;
    uint32 flags;
    uint32 nr_modules;
    uint64 modlist_paddr;
    uint64 cmdline_paddr;
    uint64 rsdp_paddr;
    uint64 memmap_paddr;
    uint32 memmap_entries;
    uint32 reserved;
} __PACKED;

struct x86_pvh_memmap_entry {
    uint64 addr;
    uint64 size;
    uint32 type;
    uint32 reserved;
} __PACKED;

struct x86_pvh_module {
    uint64 paddr;
    uint64 size;
    uint64 cmdline_paddr;
    uint64 reserved;
} __PACKED;

/* ------------------------------------------------------------------ */
/*  Bootloader memory-map parsers                                     */
/* ------------------------------------------------------------------ */

static struct mem_region x86_reserved_regions[MAX_RESERVED_REGIONS];

static int x86_parse_pvh_memory(void *boot_params, uint64 *base_out,
                                uint64 *size_out)
{
    struct x86_pvh_start_info *si = (struct x86_pvh_start_info *)boot_params;
    if (si == 0 || si->magic != 0x336ec578U || si->memmap_paddr == 0 ||
        si->memmap_entries == 0)
        return -1;

    memset(&platform, 0, sizeof(platform));
    platform.reserved = x86_reserved_regions;

    struct x86_pvh_memmap_entry *entries =
        (struct x86_pvh_memmap_entry *)(uint64)si->memmap_paddr;
    uint64 chosen_base = 0;
    uint64 chosen_size = 0;

    for (uint32 i = 0; i < si->memmap_entries; i++) {
        uint64 region_base = entries[i].addr;
        uint64 region_end  = region_base + entries[i].size;

        if (entries[i].size == 0)
            continue;

        if (entries[i].type == 1) {
            if (region_end > 0x100000ULL &&
                platform.mem_count < MAX_MEM_REGIONS) {
                if (region_base < 0x100000ULL)
                    region_base = 0x100000ULL;
                if (region_base < region_end) {
                    platform.mem[platform.mem_count].base = region_base;
                    platform.mem[platform.mem_count].size =
                        region_end - region_base;
                    platform.total_mem += region_end - region_base;
                    if (chosen_size == 0 || region_base < chosen_base) {
                        chosen_base = region_base;
                        chosen_size = region_end - region_base;
                    }
                    platform.mem_count++;
                }
            }
        } else if (platform.reserved_count < MAX_RESERVED_REGIONS) {
            platform.reserved[platform.reserved_count].base = region_base;
            platform.reserved[platform.reserved_count].size =
                region_end - region_base;
            platform.reserved_count++;
        }
    }

    if (si->nr_modules > 0 && si->modlist_paddr != 0) {
        struct x86_pvh_module *mods =
            (struct x86_pvh_module *)(uint64)si->modlist_paddr;
        for (uint32 i = 0;
             i < si->nr_modules &&
             platform.reserved_count < MAX_RESERVED_REGIONS;
             i++) {
            if (mods[i].size != 0) {
                platform.reserved[platform.reserved_count].base =
                    mods[i].paddr;
                platform.reserved[platform.reserved_count].size =
                    mods[i].size;
                platform.reserved_count++;
            }
        }

        /* First PVH module is the initrd / ramdisk (fs.img) */
        if (si->nr_modules > 0 && mods[0].size != 0) {
            platform.ramdisk_base = mods[0].paddr;
            platform.ramdisk_size = mods[0].size;
            platform.has_ramdisk = 1;
        }
    }

    /* Parse kernel command line from PVH start_info */
    if (si->cmdline_paddr != 0) {
        const char *cmdline = (const char *)(uint64)si->cmdline_paddr;
        size_t len = strnlen(cmdline, CMDLINE_MAX - 1);
        if (len > 0) {
            memcpy(platform.cmdline, cmdline, len);
            platform.cmdline[len] = '\0';
            platform.has_cmdline = 1;
        }
    }

    if (platform.mem_count == 0 || chosen_size == 0)
        return -1;

    platform.ncpu = 1;
    *base_out = chosen_base;
    *size_out = chosen_size;
    return 0;
}

static int x86_parse_bootloader_memory(void *boot_params, uint64 *base_out,
                                       uint64 *size_out)
{
    if (boot_params == 0 || base_out == 0 || size_out == 0)
        return -1;

    if (x86_parse_pvh_memory(boot_params, base_out, size_out) == 0)
        return 0;

    struct x86_multiboot_info *mbi =
        (struct x86_multiboot_info *)boot_params;
    if ((mbi->flags & (1U << 6)) == 0)
        return -1;

    memset(&platform, 0, sizeof(platform));
    platform.reserved = x86_reserved_regions;

    uint8 *mmap_ptr = (uint8 *)(uint64)mbi->mmap_addr;
    uint8 *mmap_end = mmap_ptr + mbi->mmap_length;
    uint64 chosen_base = 0;
    uint64 chosen_size = 0;

    while (mmap_ptr < mmap_end) {
        struct x86_multiboot_mmap_entry *entry =
            (struct x86_multiboot_mmap_entry *)mmap_ptr;
        uint64 region_base = entry->addr;
        uint64 region_end  = region_base + entry->len;

        if (entry->len != 0) {
            if (entry->type == 1) {
                if (region_end > 0x100000ULL &&
                    platform.mem_count < MAX_MEM_REGIONS) {
                    if (region_base < 0x100000ULL)
                        region_base = 0x100000ULL;
                    if (region_base < region_end) {
                        platform.mem[platform.mem_count].base = region_base;
                        platform.mem[platform.mem_count].size =
                            region_end - region_base;
                        platform.total_mem += region_end - region_base;
                        if (chosen_size == 0 || region_base < chosen_base) {
                            chosen_base = region_base;
                            chosen_size = region_end - region_base;
                        }
                        platform.mem_count++;
                    }
                }
            } else if (platform.reserved_count < MAX_RESERVED_REGIONS) {
                platform.reserved[platform.reserved_count].base = region_base;
                platform.reserved[platform.reserved_count].size =
                    region_end - region_base;
                platform.reserved_count++;
            }
        }

        mmap_ptr += entry->size + sizeof(entry->size);
    }

    if ((mbi->flags & (1U << 3)) && mbi->mods_count > 0) {
        struct x86_multiboot_module *mods =
            (struct x86_multiboot_module *)(uint64)mbi->mods_addr;
        for (uint32 i = 0;
             i < mbi->mods_count &&
             platform.reserved_count < MAX_RESERVED_REGIONS;
             i++) {
            if (mods[i].mod_end > mods[i].mod_start) {
                platform.reserved[platform.reserved_count].base =
                    (uint64)mods[i].mod_start;
                platform.reserved[platform.reserved_count].size =
                    (uint64)mods[i].mod_end - (uint64)mods[i].mod_start;
                platform.reserved_count++;
            }
        }

        /* First module is the initrd / ramdisk (fs.img) */
        if (mods[0].mod_end > mods[0].mod_start) {
            platform.ramdisk_base = (uint64)mods[0].mod_start;
            platform.ramdisk_size =
                (uint64)mods[0].mod_end - (uint64)mods[0].mod_start;
            platform.has_ramdisk = 1;
        }
    }

    /* Parse kernel command line from multiboot info */
    if (!platform.has_cmdline && (mbi->flags & (1U << 2)) && mbi->cmdline != 0) {
        const char *cmdline = (const char *)(uint64)mbi->cmdline;
        size_t len = strnlen(cmdline, CMDLINE_MAX - 1);
        if (len > 0) {
            memcpy(platform.cmdline, cmdline, len);
            platform.cmdline[len] = '\0';
            platform.has_cmdline = 1;
        }
    }

    if (platform.mem_count == 0 || chosen_size == 0)
        return -1;

    platform.ncpu = 1;
    *base_out = chosen_base;
    *size_out = chosen_size;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Platform interface implementation (platform.h)                    */
/* ------------------------------------------------------------------ */

uint64 platform_default_mem_base(void)
{
    return 0x00100000; /* 1 MiB — below this is legacy BIOS / ROM area */
}

int platform_early_memory(void *boot_data, uint64 *base_out, uint64 *size_out)
{
    return x86_parse_bootloader_memory(boot_data, base_out, size_out);
}

int platform_init(void *boot_data)
{
    /* x86_64 COM1 = ISA IRQ 4 (not the RISC-V default of 10) */
    __uart0_irqno = 4;

    /* PVH / multiboot parsing already populated platform in
     * platform_early_memory().  Install a fallback region if nothing was
     * found (shouldn't happen, but be safe). */
    if (platform.mem_count == 0) {
        platform.mem[0].base = 0x00100000;
        platform.mem[0].size = 128ULL * 1024 * 1024;
        platform.mem_count   = 1;
        platform.total_mem   = platform.mem[0].size;
        platform.ncpu        = 1;
    }

    uint64 first_base = platform.mem[0].base;
    uint64 first_end  = platform.mem[0].base + platform.mem[0].size;
    printf("x86 boot memory: regions=%d reserved=%d total=%ld MB "
           "first=[0x%lx-0x%lx)\n",
           platform.mem_count, platform.reserved_count,
           platform.total_mem / (1024 * 1024), first_base, first_end);
        x86_print_region_list("mem", platform.mem, platform.mem_count);
        x86_print_region_list("reserved", platform.reserved,
                     platform.reserved_count);

    if (platform.has_ramdisk) {
        printf("x86 ramdisk: 0x%lx - 0x%lx (%ld KB)\n",
               platform.ramdisk_base,
               platform.ramdisk_base + platform.ramdisk_size,
               platform.ramdisk_size / 1024);
    }

    if (platform.has_cmdline) {
        printf("x86 kernel cmdline: %s\n", platform.cmdline);
    }

    return 0;
}

void platform_apply_config(void)
{
    extern uint64 __physical_memory_start;
    extern uint64 __physical_memory_end;
    extern uint64 __physical_total_pages;

    /*
     * Split any memory region that crosses the 4 GB physical boundary into
     * two entries: the portion below 4 GB stays as low memory (region 0) and
     * the portion above becomes a separate highmem region.  This keeps the
     * page array manageable and matches the early allocator's cap done in
     * start_kernel.c.
     */
    #define PHYS_4GB 0x100000000ULL
    for (int i = 0; i < platform.mem_count; i++) {
        uint64 base = platform.mem[i].base;
        uint64 end  = base + platform.mem[i].size;
        if (base < PHYS_4GB && end > PHYS_4GB &&
            platform.mem_count < MAX_MEM_REGIONS) {
            uint64 low_size  = PHYS_4GB - base;
            uint64 high_size = end - PHYS_4GB;
            platform.mem[i].size = low_size;

            /* Shift subsequent entries to make room */
            for (int j = platform.mem_count; j > i + 1; j--)
                platform.mem[j] = platform.mem[j - 1];

            platform.mem[i + 1].base = PHYS_4GB;
            platform.mem[i + 1].size = high_size;
            platform.mem_count++;
            i++; /* skip the newly inserted entry */
        }
    }
    #undef PHYS_4GB

    /*
     * Extend __physical_memory_end to cover all memory regions, including
     * any above 4 GB.  The page array will span the entire range from
     * the first region's base to the last region's end.  Gap pages are
     * marked LOCKED during buddy init.
     */
    if (platform.mem_count > 0 && platform.mem[0].size > 0) {
        __physical_memory_start = platform.mem[0].base;

        uint64 highest_end = platform.mem[0].base + platform.mem[0].size;
        for (int i = 1; i < platform.mem_count; i++) {
            uint64 region_end = platform.mem[i].base + platform.mem[i].size;
            if (region_end > highest_end)
                highest_end = region_end;
        }
        __physical_memory_end = highest_end;
        __physical_total_pages =
            (highest_end - __physical_memory_start) >> 12;

        early_allocator_extend(PA2VA(highest_end));
    }
}

void platform_probe_extensions(void)
{
    /* No-op — no SBI on x86. */
}

void platform_print_mem_summary(void)
{
    x86_debug_boot_mem_summary();

    if (platform.mem_count > 0) {
        uint64 first_base = platform.mem[0].base;
        uint64 first_end = platform.mem[0].base + platform.mem[0].size;
        printf("x86 boot memory: regions=%d reserved=%d total=%ld MB first=[0x%lx-0x%lx)\n",
               platform.mem_count, platform.reserved_count,
               platform.total_mem / (1024 * 1024), first_base, first_end);
    }

    x86_print_region_list("mem", platform.mem, platform.mem_count);
    x86_print_region_list("reserved", platform.reserved,
                          platform.reserved_count);

    if (platform.has_ramdisk) {
        printf("x86 ramdisk: [0x%lx-0x%lx) size=0x%lx\n",
               platform.ramdisk_base,
               platform.ramdisk_base + platform.ramdisk_size,
               platform.ramdisk_size);
    }

    if (platform.has_cmdline) {
        printf("x86 kernel cmdline: %s\n", platform.cmdline);
    }

    {
        const uint64 low_identity_top = 4ULL << 30;

        x86_print_kvm_region("kvm low identity", KVMBASE, low_identity_top);
        x86_print_kvm_region("mmio window", 0xFE000000ULL, 0xFF000000ULL);

        for (int i = 0; i < platform.mem_count; i++) {
            uint64 base = platform.mem[i].base;
            uint64 end = base + platform.mem[i].size;
            if (end <= low_identity_top)
                continue;
            if (base < low_identity_top)
                base = low_identity_top;
            x86_print_kvm_region("high ram", base, end);
        }

        for (int i = 0; i < platform.reserved_count; i++) {
            uint64 base = platform.reserved[i].base;
            uint64 end = base + platform.reserved[i].size;
            if (end <= low_identity_top)
                continue;
            if (base < low_identity_top)
                base = low_identity_top;
            x86_print_kvm_region("high reserved/mmio", base, end);
        }
    }
}

void platform_post_vm_init(void)
{
    /* Paging is already on from the PVH entry stub.
     * Reserve all pre-mapped low-kernel identity space, including the
     * low RAM aperture, firmware/reserved regions, and static MMIO window,
     * so kvm_mmap() never places dynamic kernel VM allocations there. */
    const uint64 low_identity_top = 4ULL << 30;

    x86_print_kvm_region("kvm low identity", KVMBASE, low_identity_top);
    x86_print_kvm_region("mmio window", 0xFE000000ULL, 0xFF000000ULL);

    if (kvm_register_region(KVMBASE, low_identity_top - KVMBASE,
                            PROT_READ | PROT_WRITE) != 0)
        panic("platform_post_vm_init: reserve low identity aperture failed");

    for (int i = 0; i < platform.mem_count; i++) {
        uint64 base = platform.mem[i].base;
        uint64 end = base + platform.mem[i].size;
        if (end <= low_identity_top)
            continue;
        if (base < low_identity_top)
            base = low_identity_top;
        x86_print_kvm_region("high ram", base, end);
        if (kvm_register_region(base, end - base,
                                PROT_READ | PROT_WRITE) != 0)
            panic("platform_post_vm_init: reserve high RAM region failed");
    }

    for (int i = 0; i < platform.reserved_count; i++) {
        uint64 base = platform.reserved[i].base;
        uint64 end = base + platform.reserved[i].size;
        if (end <= low_identity_top)
            continue;
        if (base < low_identity_top)
            base = low_identity_top;
        x86_print_kvm_region("high reserved/mmio", base, end);
        if (kvm_register_region(base, end - base,
                                PROT_READ | PROT_WRITE) != 0)
            panic("platform_post_vm_init: reserve high reserved region failed");
    }
}

int platform_boot_cpu_limit(void)
{
    if (x86_boot_cpu_limit_cached == 0)
        x86_boot_cpu_limit_cached = x86_detect_boot_cpu_limit();
    return x86_boot_cpu_limit_cached;
}

void platform_prepare_current_cpu_stack(void)
{
    int cpu = cpuid();
    int limit = platform_boot_cpu_limit();

    if (cpu < 0 || cpu >= limit)
        panic("platform_prepare_current_cpu_stack: invalid CPU %d", cpu);

    if (x86_kernel_stacks != NULL && x86_kernel_stacks[cpu] != NULL)
        return;

    uint64 stack_top = x86_kernel_stack_top(cpu);
    printf("[SMP] CPU %d dynamic kernel stack: 0x%lx-0x%lx\n",
           cpu, stack_top - KERNEL_STACK_SIZE, stack_top);
    x86_relocate_current_stack(stack_top);
}

void platform_start_secondary_cpus(uint64 entry)
{
    (void)entry;

    /*
     * AP boot trampoline — copy to low memory and send INIT-SIPI-SIPI.
     *
     * The trampoline code (ap_trampoline.S) is linked into .rodata and
     * copied to AP_BOOT_PA.  Before copying we patch the data area with
     * the kernel CR3, per-CPU boot stack tops, and start_kernel address.
     * The stack table is allocator-backed and sized by platform_boot_cpu_limit().
     */
    #define AP_BOOT_PA  0x8000

    extern uint8 ap_trampoline_start[];
    extern uint8 ap_trampoline_end[];
    extern uint64 ap_boot_cr3;
    extern uint64 ap_boot_target;
    extern uint64 ap_boot_cpus_base;
    extern uint32 ap_boot_cpu_stride;
    extern uint64 ap_boot_stack_tops_base;
    extern uint32 ap_boot_cpu_limit;
    extern uint32 ap_boot_next_cpu;

    extern void start_kernel(int, void *, bool);
    extern uint64 kernel_pagetable;
    extern struct cpu_local cpus[];

    int cpu_limit = platform_boot_cpu_limit();
    if (cpu_limit <= 1) {
        platform.ncpu = 1;
        printf("[SMP] CPU topology reports one CPU; skipping AP startup\n");
        return;
    }

    if (x86_ap_boot_stack_tops == NULL) {
        x86_ap_boot_stack_tops = page_alloc(0, PAGE_TYPE_ANON);
        if (x86_ap_boot_stack_tops == NULL)
            panic("platform_start_secondary_cpus: stack table allocation failed");
    }
    memset(x86_ap_boot_stack_tops, 0, PAGE_SIZE);
    for (int cpu = 1; cpu < cpu_limit; cpu++)
        x86_ap_boot_stack_tops[cpu] = x86_kernel_stack_top(cpu);

    uint64 tramp_size = (uint64)ap_trampoline_end - (uint64)ap_trampoline_start;
    if (tramp_size > 4096)
        panic("AP trampoline too large");

    /* Patch data area in the source copy (rodata is identity-mapped, writable
     * in kernel pagetable) */
    ap_boot_cr3 = (uint64)kernel_pagetable;
    ap_boot_target = (uint64)start_kernel;
    ap_boot_cpus_base = (uint64)&cpus[0];
    ap_boot_cpu_stride = (uint32)sizeof(struct cpu_local);
    ap_boot_stack_tops_base = (uint64)x86_ap_boot_stack_tops;
    ap_boot_cpu_limit = (uint32)cpu_limit;
    ap_boot_next_cpu = 1;           /* BSP is 0; APs atomically claim 1,2,3... */

    /* Copy trampoline to low memory */
    memmove((void *)(uint64)AP_BOOT_PA, ap_trampoline_start, tramp_size);

    printf("[SMP] Starting secondary CPUs (prepared=%d, trampoline at 0x%x, size %d)\n",
           cpu_limit, AP_BOOT_PA, (int)tramp_size);

    /*
     * INIT-SIPI-SIPI sequence (broadcast to all APs):
     *
     * 1. Send INIT IPI to all-except-self
     * 2. Wait 10 ms
     * 3. Send STARTUP IPI (vector = AP_BOOT_PA >> 12) to all-except-self
     * 4. Wait 200 us
     * 5. Send second STARTUP IPI (for reliability)
     * 6. Wait 200 us
     */

    /* Wait for LAPIC ICR to be idle */
    while (lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_STATUS)
        ;

    /* INIT IPI: all-except-self, level assert */
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO,
                LAPIC_ICR_INIT | LAPIC_ICR_LEVEL |
                (3 << 18));  /* destination shorthand: all-excl-self */

    /* Wait ~10 ms (busy-loop using port 0x80 delay, ~1us each) */
    for (volatile int i = 0; i < 10000; i++)
        asm volatile("outb %%al, $0x80" : : "a"(0));

    /* Send STARTUP IPI twice */
    for (int sipi = 0; sipi < 2; sipi++) {
        while (lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_STATUS)
            ;

        lapic_write(LAPIC_ICR_HI, 0);
        lapic_write(LAPIC_ICR_LO,
                    LAPIC_ICR_STARTUP |
                    (AP_BOOT_PA >> 12) |       /* vector = page number */
                    (3 << 18));                 /* all-excl-self */

        /* Wait ~200 us */
        for (volatile int i = 0; i < 200; i++)
            asm volatile("outb %%al, $0x80" : : "a"(0));
    }

    /* Wait a bit for APs to increment ap_boot_next_cpu */
    for (volatile int i = 0; i < 50000; i++)
        asm volatile("outb %%al, $0x80" : : "a"(0));

    /* ap_boot_next_cpu is consumed by the copied trampoline at AP_BOOT_PA,
     * not by the original image symbol. Read back the runtime copy. */
    uint32 *runtime_ap_boot_next_cpu =
        (uint32 *)(AP_BOOT_PA +
                   ((uint64)&ap_boot_next_cpu -
                    (uint64)ap_trampoline_start));

    /* ap_boot_next_cpu started at 1 (BSP=0). After APs boot it equals
     * the total number of CPUs (BSP + APs). */
    uint32 claimed_cpus =
        __atomic_load_n(runtime_ap_boot_next_cpu, __ATOMIC_ACQUIRE);
    if (claimed_cpus > (uint32)cpu_limit) {
        printf("[SMP] %d CPUs attempted startup; only %d slots prepared\n",
               claimed_cpus, cpu_limit);
        claimed_cpus = (uint32)cpu_limit;
    }
    if (claimed_cpus < 1)
        claimed_cpus = 1;
    platform.ncpu = claimed_cpus;
    printf("[SMP] %d CPUs online (1 BSP + %d APs, prepared %d)\n",
           claimed_cpus, claimed_cpus - 1, cpu_limit);
}

void platform_secondary_cpu_init(void)
{
    /* No-op for now (single-CPU). */
}

void platform_start_per_cpu_services(int cpu)
{
    rcu_kthread_start_cpu(cpu);
}

void platform_late_device_init(void)
{
    sched_timer_init();

    /* Enumerate PCI bus: discovers e1000 NIC and virtio-blk-pci devices.
     * Sets platform.has_virtio if virtio devices are found. */
    extern void pci_init(void);
    pci_init();
}

void platform_boot_mark(const char *msg)
{
    x86_debug_puts(msg);
}

/**
 * Write a 16-bit value to an I/O port.
 * Needed for ACPI PM1a control register (shutdown).
 */
static inline void x86_outw(uint16 port, uint16 value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void platform_shutdown(void)
{
    printf("System shutting down...\n");

    /* QEMU PIIX4/Q35 ACPI shutdown:
     * Write SLP_TYP=5 | SLP_EN to PM1a control register at port 0x604.
     * This causes QEMU to exit cleanly. */
    x86_outw(0x604, 0x2000);

    /* Fallback: spin with HLT if ACPI shutdown didn't work */
    for (;;)
        asm volatile("hlt");
}

void platform_reboot(void)
{
    printf("System rebooting...\n");

    /* Keyboard controller reset via PS/2 port:
     * Writing 0xFE to port 0x64 pulses the CPU reset line. */
    x86_debug_outb(0x64, 0xFE);

    /* Fallback: spin with HLT if keyboard reset didn't work */
    for (;;)
        asm volatile("hlt");
}
