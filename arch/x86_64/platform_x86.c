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
#include "param.h"
#include "mm/page.h"
#include "smp/percpu.h"

/* Global platform info structure (RISC-V defines this in fdt.c) */
struct platform_info platform;

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

    if (platform.has_ramdisk) {
        printf("x86 ramdisk: 0x%lx - 0x%lx (%ld KB)\n",
               platform.ramdisk_base,
               platform.ramdisk_base + platform.ramdisk_size,
               platform.ramdisk_size / 1024);
    }

    return 0;
}

void platform_apply_config(void)
{
    /* No-op — ACPI / PCI enumeration will go here eventually. */
}

void platform_probe_extensions(void)
{
    /* No-op — no SBI on x86. */
}

void platform_print_mem_summary(void)
{
    x86_debug_boot_mem_summary();
}

void platform_post_vm_init(void)
{
    /* Paging is already on from the PVH entry stub. */
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
     *
     * Stacks are pre-reserved for ALL secondary cores before SIPI is sent.
     * Each AP atomically increments ap_boot_next_cpu to claim a unique
     * hartid, then uses that to index into ap_boot_stacks[].
     */
    #define AP_BOOT_PA  0x8000

    extern uint8 ap_trampoline_start[];
    extern uint8 ap_trampoline_end[];
    extern uint64 ap_boot_cr3;
    extern uint64 ap_boot_target;
    extern uint64 ap_boot_cpus_base;
    extern uint32 ap_boot_cpu_stride;
    extern uint64 ap_boot_stacks_base;
    extern uint32 ap_boot_next_cpu;

    extern void start_kernel(int, void *, bool);
    extern uint64 kernel_pagetable;
    extern struct cpu_local cpus[];
    extern char boot_stacks[];          /* entry.S: KERNEL_STACK_SIZE * NCPU */

    uint64 tramp_size = (uint64)ap_trampoline_end - (uint64)ap_trampoline_start;
    if (tramp_size > 4096)
        panic("AP trampoline too large");

    /* Patch data area in the source copy (rodata is identity-mapped, writable
     * in kernel pagetable) */
    ap_boot_cr3 = (uint64)kernel_pagetable;
    ap_boot_target = (uint64)start_kernel;
    ap_boot_cpus_base = (uint64)&cpus[0];
    ap_boot_cpu_stride = (uint32)sizeof(struct cpu_local);
    ap_boot_stacks_base = (uint64)boot_stacks; /* static array from entry.S */
    ap_boot_next_cpu = 1;           /* BSP is 0; APs atomically claim 1,2,3... */

    /* Stacks are pre-reserved in the static boot_stacks array (entry.S).
     * Each AP computes its stack from boot_stacks + (hartid+1) * KERNEL_STACK_SIZE.
     * No runtime allocation needed. */

    /* Copy trampoline to low memory */
    memmove((void *)(uint64)AP_BOOT_PA, ap_trampoline_start, tramp_size);

    printf("[SMP] Starting secondary CPUs (trampoline at 0x%x, size %d)\n",
           AP_BOOT_PA, (int)tramp_size);

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

    /* ap_boot_next_cpu started at 1 (BSP=0). After APs boot it equals
     * the total number of CPUs (BSP + APs). */
    uint32 total_cpus = __atomic_load_n(&ap_boot_next_cpu, __ATOMIC_ACQUIRE);
    platform.ncpu = total_cpus;
    printf("[SMP] %d CPUs online (1 BSP + %d APs)\n", total_cpus, total_cpus - 1);
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
