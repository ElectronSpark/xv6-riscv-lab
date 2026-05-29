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
static const char *x86_mem_source = "fallback";
static uint32 x86_cpu_apic_ids[MAX_CPUS];
static int x86_cpu_apic_id_count;
static int x86_acpi_cpu_ids_loaded;
static int x86_acpi_pci_diag_logged;

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

struct acpi_rsdp {
    char  signature[8];
    uint8 checksum;
    char  oemid[6];
    uint8 revision;
    uint32 rsdt_address;
    uint32 length;
    uint64 xsdt_address;
    uint8 extended_checksum;
    uint8 reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char  signature[4];
    uint32 length;
    uint8 revision;
    uint8 checksum;
    char  oemid[6];
    char  oem_table_id[8];
    uint32 oem_revision;
    uint32 creator_id;
    uint32 creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32 lapic_address;
    uint32 flags;
    uint8 entries[];
} __attribute__((packed));

static int x86_cpu_limit_capacity(void)
{
    int cap = MAX_CPUS;
    if (cap > MAX_CPUS)
        cap = MAX_CPUS;
    if (cap < 1)
        cap = 1;
    return cap;
}

static uint64 x86_cmdline_u64(const char *key)
{
    size_t key_len = strlen(key);
    const char *p = platform.cmdline;

    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            uint64 value = 0;
            p += key_len + 1;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
                p += 2;
            while ((*p >= '0' && *p <= '9') ||
                   (*p >= 'a' && *p <= 'f') ||
                   (*p >= 'A' && *p <= 'F')) {
                uint64 digit;
                if (*p >= '0' && *p <= '9')
                    digit = (uint64)(*p - '0');
                else if (*p >= 'a' && *p <= 'f')
                    digit = (uint64)(*p - 'a' + 10);
                else
                    digit = (uint64)(*p - 'A' + 10);
                value = (value << 4) | digit;
                p++;
            }
            return value;
        }
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
    }

    return 0;
}

static int x86_acpi_checksum_ok(const void *base, uint32 len)
{
    const uint8 *p = (const uint8 *)base;
    uint8 sum = 0;

    if (base == NULL || len == 0)
        return 0;
    for (uint32 i = 0; i < len; i++)
        sum = (uint8)(sum + p[i]);
    return sum == 0;
}

static int x86_acpi_sig_eq(const char got[4], const char want[4])
{
    return got[0] == want[0] && got[1] == want[1] &&
           got[2] == want[2] && got[3] == want[3];
}

static uint16 x86_acpi_read_u16(const void *base, uint32 off)
{
    uint16 val;
    memcpy(&val, ((const uint8 *)base) + off, sizeof(val));
    return val;
}

static uint32 x86_acpi_read_u32(const void *base, uint32 off)
{
    uint32 val;
    memcpy(&val, ((const uint8 *)base) + off, sizeof(val));
    return val;
}

static uint64 x86_acpi_read_u64(const void *base, uint32 off)
{
    uint64 val;
    memcpy(&val, ((const uint8 *)base) + off, sizeof(val));
    return val;
}

static int x86_acpi_table_ok(const struct acpi_sdt_header *hdr)
{
    return hdr != NULL && hdr->length >= sizeof(*hdr) &&
           x86_acpi_checksum_ok(hdr, hdr->length);
}

static int x86_acpi_blob_has_ascii(const uint8 *base, uint32 len,
                                   const char *needle)
{
    size_t needle_len = strlen(needle);

    if (base == NULL || needle_len == 0 || len < needle_len)
        return 0;
    for (uint32 i = 0; i <= len - needle_len; i++) {
        if (memcmp(base + i, needle, needle_len) == 0)
            return 1;
    }
    return 0;
}

static int x86_acpi_blob_has_u32(const uint8 *base, uint32 len, uint32 needle)
{
    if (base == NULL || len < sizeof(uint32))
        return 0;
    for (uint32 i = 0; i <= len - sizeof(uint32); i++) {
        if (x86_acpi_read_u32(base, i) == needle)
            return 1;
    }
    return 0;
}

static void x86_acpi_print_sig(const char sig[4])
{
    printf("%c%c%c%c", sig[0], sig[1], sig[2], sig[3]);
}

static void x86_acpi_scan_pci_root_hids(const struct acpi_sdt_header *hdr,
                                        int *pnp0a03, int *pnp0a08)
{
    int has_pnp0a03;
    int has_pnp0a08;

    if (!x86_acpi_table_ok(hdr))
        return;

    has_pnp0a03 = x86_acpi_blob_has_ascii((const uint8 *)hdr, hdr->length,
                                          "PNP0A03") ||
                  x86_acpi_blob_has_u32((const uint8 *)hdr, hdr->length,
                                        0x030AD041);
    has_pnp0a08 = x86_acpi_blob_has_ascii((const uint8 *)hdr, hdr->length,
                                          "PNP0A08") ||
                  x86_acpi_blob_has_u32((const uint8 *)hdr, hdr->length,
                                        0x080AD041);
    if (has_pnp0a03) {
        (*pnp0a03)++;
        printf("ACPI: PCI root HID PNP0A03 present in ");
        x86_acpi_print_sig(hdr->signature);
        printf("\n");
    }
    if (has_pnp0a08) {
        (*pnp0a08)++;
        printf("ACPI: PCI root HID PNP0A08 present in ");
        x86_acpi_print_sig(hdr->signature);
        printf("\n");
    }
}

static void x86_acpi_scan_mmio_windows(const struct acpi_sdt_header *hdr);

static void x86_acpi_scan_facp_dsdt(const struct acpi_sdt_header *facp,
                                    int *pnp0a03, int *pnp0a08)
{
    uint64 dsdt_addr = 0;
    const struct acpi_sdt_header *dsdt;

    if (!x86_acpi_table_ok(facp))
        return;

    if (facp->length >= 148)
        dsdt_addr = x86_acpi_read_u64(facp, 140);
    if (dsdt_addr == 0 && facp->length >= 44)
        dsdt_addr = x86_acpi_read_u32(facp, 40);
    if (dsdt_addr == 0 || dsdt_addr >= 0x100000000ULL) {
        printf("ACPI: FACP DSDT pointer unavailable for PCI root HID scan\n");
        return;
    }

    dsdt = (const struct acpi_sdt_header *)dsdt_addr;
    if (dsdt == NULL || !x86_acpi_sig_eq(dsdt->signature, "DSDT")) {
        printf("ACPI: FACP DSDT pointer did not reference DSDT\n");
        return;
    }
    x86_acpi_scan_pci_root_hids(dsdt, pnp0a03, pnp0a08);
    x86_acpi_scan_mmio_windows(dsdt);
}

/*
 * Record a high-MMIO producer window discovered in an ACPI resource template.
 * Values are copied verbatim from the real ACPI descriptor; ranges below 4GB
 * (low MMIO gap) are ignored because assigned-device (DDA) BARs for large GPU
 * apertures live in the high window the host advertises. Duplicates and zero
 * lengths are dropped.
 */
static void x86_acpi_store_high_mmio(uint64 base, uint64 len)
{
    if (len == 0 || base < 0x100000000ULL)
        return;
    if (base + len < base)
        return; /* overflow */
    for (int i = 0; i < platform.high_mmio_count; i++) {
        if (platform.high_mmio[i].base == base &&
            platform.high_mmio[i].size == len)
            return;
    }
    if (platform.high_mmio_count >= ACPI_HIGH_MMIO_MAX)
        return;
    platform.high_mmio[platform.high_mmio_count].base = base;
    platform.high_mmio[platform.high_mmio_count].size = len;
    platform.high_mmio_count++;
    printf("ACPI: high MMIO window base=0x%lx size=0x%lx (DDA BAR pool)\n",
           base, len);
}

/*
 * Scan an ACPI table blob (DSDT/SSDT) for QWord Address Space Descriptors
 * (large resource tag 0x8A) describing memory producer windows above 4GB.
 * On Hyper-V Gen2 VMs the VMBus root _CRS exposes the high-MMIO aperture this
 * way; the same descriptors back the window configured by the host's
 * -HighMemoryMappedIoSpace setting. The scan validates the descriptor layout
 * to avoid matching unrelated AML bytes.
 */
static void x86_acpi_scan_mmio_windows(const struct acpi_sdt_header *hdr)
{
    const uint8 *base;
    uint32 len;

    if (!x86_acpi_table_ok(hdr))
        return;
    base = (const uint8 *)hdr;
    len = hdr->length;
    if (len < sizeof(*hdr) + 46)
        return;

    for (uint32 i = sizeof(*hdr); i + 46 <= len; i++) {
        uint16 dlen;
        uint8 res_type;
        uint64 addr_min;
        uint64 addr_max;
        uint64 addr_len;

        if (base[i] != 0x8A)
            continue;
        /* Large resource length covers bytes after the 3-byte tag+length. */
        dlen = x86_acpi_read_u16(base, i + 1);
        if (dlen < 43 || (uint64)i + 3 + dlen > len)
            continue;
        res_type = base[i + 3];
        if (res_type != 0) /* 0 == memory range */
            continue;
        addr_min = x86_acpi_read_u64(base, i + 14);
        addr_max = x86_acpi_read_u64(base, i + 22);
        addr_len = x86_acpi_read_u64(base, i + 38);

        /* Derive length when the descriptor only fixes the address range. */
        if (addr_len == 0 && addr_max >= addr_min)
            addr_len = addr_max - addr_min + 1;
        if (addr_len == 0 || addr_min < 0x100000000ULL)
            continue;
        /* Sanity: aligned and self-consistent (min..max bounds the length). */
        if ((addr_min & 0xFFFULL) != 0)
            continue;
        if (addr_max >= addr_min && addr_len > (addr_max - addr_min + 1))
            continue;
        x86_acpi_store_high_mmio(addr_min, addr_len);
    }
}

static int x86_acpi_log_mcfg(const struct acpi_sdt_header *mcfg)
{
    enum {
        MCFG_RESERVED_OFFSET = sizeof(struct acpi_sdt_header),
        MCFG_ALLOCATION_OFFSET = sizeof(struct acpi_sdt_header) + 8,
        MCFG_ALLOCATION_SIZE = 16,
    };
    uint32 off = MCFG_ALLOCATION_OFFSET;
    int entries = 0;

    if (!x86_acpi_table_ok(mcfg) || mcfg->length < MCFG_ALLOCATION_OFFSET) {
        printf("ACPI: MCFG present but invalid\n");
        return 1;
    }

    while (off + MCFG_ALLOCATION_SIZE <= mcfg->length) {
        uint64 ecam_base = x86_acpi_read_u64(mcfg, off);
        uint16 segment = x86_acpi_read_u16(mcfg, off + 8);
        uint8 start_bus = ((const uint8 *)mcfg)[off + 10];
        uint8 end_bus = ((const uint8 *)mcfg)[off + 11];

        printf("ACPI: MCFG entry %d ecam=0x%lx segment=%d bus=%d-%d\n",
               entries, ecam_base, segment, start_bus, end_bus);
        entries++;
        off += MCFG_ALLOCATION_SIZE;
    }

    printf("ACPI: MCFG present entries=%d reserved=0x%lx\n", entries,
           x86_acpi_read_u64(mcfg, MCFG_RESERVED_OFFSET));
    return 1;
}

static void x86_acpi_log_pci_diagnostics(const struct acpi_sdt_header *root,
                                         int use_xsdt)
{
    uint32 entries;
    int mcfg_count = 0;
    int facp_count = 0;
    int ssdt_count = 0;
    int skipped_high = 0;
    int pnp0a03 = 0;
    int pnp0a08 = 0;

    if (x86_acpi_pci_diag_logged)
        return;
    x86_acpi_pci_diag_logged = 1;

    entries = (root->length - sizeof(*root)) / (use_xsdt ? 8 : 4);
    printf("ACPI: PCI diagnostics root=%s entries=%d\n",
           use_xsdt ? "XSDT" : "RSDT", entries);

    for (uint32 i = 0; i < entries; i++) {
        uint64 addr;
        const struct acpi_sdt_header *hdr;

        if (use_xsdt)
            addr = ((const uint64 *)(root + 1))[i];
        else
            addr = ((const uint32 *)(root + 1))[i];
        if (addr == 0)
            continue;
        if (addr >= 0x100000000ULL) {
            skipped_high++;
            continue;
        }

        hdr = (const struct acpi_sdt_header *)addr;
        if (hdr == NULL || hdr->length < sizeof(*hdr))
            continue;

        if (x86_acpi_sig_eq(hdr->signature, "MCFG"))
            mcfg_count += x86_acpi_log_mcfg(hdr);
        else if (x86_acpi_sig_eq(hdr->signature, "FACP")) {
            facp_count++;
            x86_acpi_scan_facp_dsdt(hdr, &pnp0a03, &pnp0a08);
        } else if (x86_acpi_sig_eq(hdr->signature, "SSDT")) {
            ssdt_count++;
            x86_acpi_scan_pci_root_hids(hdr, &pnp0a03, &pnp0a08);
            x86_acpi_scan_mmio_windows(hdr);
        }
    }

    if (mcfg_count == 0)
        printf("ACPI: MCFG absent from root table\n");
    printf("ACPI: PCI diagnostics summary MCFG=%d FACP=%d SSDT=%d skipped_hi=%d PNP0A03=%d PNP0A08=%d\n",
           mcfg_count, facp_count, ssdt_count, skipped_high, pnp0a03,
           pnp0a08);
}

static int x86_count_madt_cpus(const struct acpi_madt *madt, uint32 *ids,
                               int max_ids)
{
    uint32 off = sizeof(*madt);
    int count = 0;

    if (madt == NULL || madt->header.length < sizeof(*madt) ||
        !x86_acpi_checksum_ok(madt, madt->header.length))
        return 0;

    while (off + 2 <= madt->header.length) {
        const uint8 *entry = ((const uint8 *)madt) + off;
        uint8 type = entry[0];
        uint8 len = entry[1];

        if (len < 2 || off + len > madt->header.length)
            break;

        if (type == 0 && len >= 8) {
            uint8 apic_id = entry[3];
            uint32 flags = *(const uint32 *)(entry + 4);
            if (flags & 0x3) {
                if (ids != NULL && count < max_ids)
                    ids[count] = apic_id;
                count++;
            }
        } else if (type == 9 && len >= 16) {
            uint32 apic_id = *(const uint32 *)(entry + 4);
            uint32 flags = *(const uint32 *)(entry + 12);
            if (flags & 0x3) {
                if (ids != NULL && count < max_ids)
                    ids[count] = apic_id;
                count++;
            }
        }

        off += len;
    }

    return count;
}

static int x86_detect_acpi_cpu_count(void)
{
    x86_acpi_cpu_ids_loaded = 1;
    x86_cpu_apic_id_count = 0;

    uint64 rsdp_addr = x86_cmdline_u64("acpi_rsdp");
    const struct acpi_rsdp *rsdp = (const struct acpi_rsdp *)rsdp_addr;
    uint64 root_addr;
    const struct acpi_sdt_header *root;
    int use_xsdt;
    uint32 entries;

    if (rsdp_addr == 0 || rsdp_addr >= 0x100000000ULL ||
        memcmp(rsdp->signature, "RSD PTR ", 8) != 0 ||
        !x86_acpi_checksum_ok(rsdp, 20))
        return 0;

    use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0 &&
               rsdp->length >= sizeof(*rsdp) &&
               x86_acpi_checksum_ok(rsdp, rsdp->length);
    root_addr = use_xsdt ? rsdp->xsdt_address : (uint64)rsdp->rsdt_address;
    if (root_addr == 0 || root_addr >= 0x100000000ULL)
        return 0;
    root = (const struct acpi_sdt_header *)root_addr;
    if (root == NULL || root->length < sizeof(*root) ||
        !x86_acpi_checksum_ok(root, root->length))
        return 0;

    if (use_xsdt) {
        if (!x86_acpi_sig_eq(root->signature, "XSDT"))
            return 0;
        x86_acpi_log_pci_diagnostics(root, use_xsdt);
        entries = (root->length - sizeof(*root)) / 8;
        const uint64 *table = (const uint64 *)(root + 1);
        for (uint32 i = 0; i < entries; i++) {
            if (table[i] == 0 || table[i] >= 0x100000000ULL)
                continue;
            const struct acpi_sdt_header *hdr =
                (const struct acpi_sdt_header *)table[i];
            if (hdr && x86_acpi_sig_eq(hdr->signature, "APIC")) {
                int count = x86_count_madt_cpus((const struct acpi_madt *)hdr,
                                                x86_cpu_apic_ids, MAX_CPUS);
                x86_cpu_apic_id_count = count > MAX_CPUS ? MAX_CPUS : count;
                return count;
            }
        }
    } else {
        if (!x86_acpi_sig_eq(root->signature, "RSDT"))
            return 0;
        x86_acpi_log_pci_diagnostics(root, use_xsdt);
        entries = (root->length - sizeof(*root)) / 4;
        const uint32 *table = (const uint32 *)(root + 1);
        for (uint32 i = 0; i < entries; i++) {
            if (table[i] == 0 || table[i] >= 0x100000000ULL)
                continue;
            const struct acpi_sdt_header *hdr =
                (const struct acpi_sdt_header *)(uint64)table[i];
            if (hdr && x86_acpi_sig_eq(hdr->signature, "APIC")) {
                int count = x86_count_madt_cpus((const struct acpi_madt *)hdr,
                                                x86_cpu_apic_ids, MAX_CPUS);
                x86_cpu_apic_id_count = count > MAX_CPUS ? MAX_CPUS : count;
                return count;
            }
        }
    }

    return 0;
}

static int x86_detect_boot_cpu_limit(void)
{
    uint32 a, b, c, d;
    uint32 count = (uint32)x86_cmdline_u64("acpi_cpus");
    int cap = x86_cpu_limit_capacity();
    uint32 acpi_count = 0;

    if (!x86_acpi_cpu_ids_loaded)
        acpi_count = (uint32)x86_detect_acpi_cpu_count();
    else
        acpi_count = (uint32)x86_cpu_apic_id_count;

    if (count == 0)
        count = acpi_count;

    x86_cpuid(0, 0, &a, &b, &c, &d);
    uint32 max_leaf = a;

    if (count == 0 && max_leaf >= 0x1f)
        count = x86_cpuid_topology_count(0x1f);
    if (count == 0 && max_leaf >= 0x0b)
        count = x86_cpuid_topology_count(0x0b);
    if (count == 0 && max_leaf >= 1) {
        x86_cpuid(1, 0, &a, &b, &c, &d);
        count = (b >> 16) & 0xff;
    }
    if (count == 0)
        count = 1;

    if ((int)count > cap) {
        printf("[SMP] CPU topology reports %d CPUs; clamping to capacity=%d\n",
               count, cap);
        count = (uint32)cap;
    }

    return (int)count;
}

int x86_cpu_apic_id(int cpu)
{
    if (cpu >= 0 && cpu < x86_cpu_apic_id_count)
        return (int)x86_cpu_apic_ids[cpu];
    return cpu;
}

static void x86_prepare_cpu_apic_ids(int cpu_limit)
{
    int bsp_apic = lapic_id();

    if (!x86_acpi_cpu_ids_loaded)
        (void)x86_detect_acpi_cpu_count();

    if (x86_cpu_apic_id_count <= 0) {
        for (int i = 0; i < cpu_limit && i < MAX_CPUS; i++)
            x86_cpu_apic_ids[i] = (uint32)i;
        x86_cpu_apic_id_count = cpu_limit;
    }

    for (int i = 0; i < x86_cpu_apic_id_count; i++) {
        if ((int)x86_cpu_apic_ids[i] == bsp_apic) {
            uint32 tmp = x86_cpu_apic_ids[0];
            x86_cpu_apic_ids[0] = x86_cpu_apic_ids[i];
            x86_cpu_apic_ids[i] = tmp;
            break;
        }
    }

    for (int i = x86_cpu_apic_id_count; i < cpu_limit && i < MAX_CPUS; i++)
        x86_cpu_apic_ids[i] = (uint32)i;
    if (x86_cpu_apic_id_count < cpu_limit)
        x86_cpu_apic_id_count = cpu_limit;

    printf("[SMP] LAPIC IDs:");
    for (int i = 0; i < cpu_limit; i++)
        printf(" cpu%d=%d", i, x86_cpu_apic_id(i));
    printf("\n");
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

struct x86_linux_e820_entry {
    uint64 addr;
    uint64 size;
    uint32 type;
} __PACKED;

#define X86_EARLY_MEM_LIMIT 0x100000000ULL
#define X86_MAX_PROBED_PA   (512ULL << 30)

/* ------------------------------------------------------------------ */
/*  Bootloader memory-map parsers                                     */
/* ------------------------------------------------------------------ */

static struct mem_region x86_reserved_regions[MAX_RESERVED_REGIONS];

static void x86_normalize_mem_regions(void)
{
    int out = 0;

    for (int i = 0; i < platform.mem_count; i++) {
        uint64 base = platform.mem[i].base;
        uint64 end = base + platform.mem[i].size;

        if (platform.mem[i].size == 0 || end <= base ||
            base >= X86_MAX_PROBED_PA)
            continue;
        if (end > X86_MAX_PROBED_PA)
            end = X86_MAX_PROBED_PA;

        platform.mem[out].base = base;
        platform.mem[out].size = end - base;
        out++;
    }
    platform.mem_count = out;

    for (int i = 0; i < platform.mem_count; i++) {
        for (int j = i + 1; j < platform.mem_count; j++) {
            if (platform.mem[j].base < platform.mem[i].base) {
                struct mem_region tmp = platform.mem[i];
                platform.mem[i] = platform.mem[j];
                platform.mem[j] = tmp;
            }
        }
    }

    out = 0;
    platform.total_mem = 0;
    for (int i = 0; i < platform.mem_count; i++) {
        uint64 base = platform.mem[i].base;
        uint64 end = base + platform.mem[i].size;

        if (out > 0) {
            uint64 prev_base = platform.mem[out - 1].base;
            uint64 prev_end = prev_base + platform.mem[out - 1].size;
            if (base <= prev_end) {
                if (end > prev_end)
                    platform.mem[out - 1].size = end - prev_base;
                continue;
            }
        }

        platform.mem[out].base = base;
        platform.mem[out].size = end - base;
        out++;
    }
    platform.mem_count = out;

    for (int i = 0; i < platform.mem_count; i++)
        platform.total_mem += platform.mem[i].size;
}

static uint8 x86_boot_u8(uint8 *base, uint32 off)
{
    return base[off];
}

static uint16 x86_boot_u16(uint8 *base, uint32 off)
{
    uint16 val;
    memcpy(&val, base + off, sizeof(val));
    return val;
}

static uint32 x86_boot_u32(uint8 *base, uint32 off)
{
    uint32 val;
    memcpy(&val, base + off, sizeof(val));
    return val;
}

static void x86_parse_linux_framebuffer(uint8 *params)
{
    enum {
        VIDEO_TYPE_VLFB = 0x23,
        VIDEO_TYPE_EFI  = 0x70,
        VIDEO_CAPABILITY_64BIT_BASE = 1 << 1,
    };

    uint8 video_type = x86_boot_u8(params, 0x0f);
    if (video_type != VIDEO_TYPE_VLFB && video_type != VIDEO_TYPE_EFI)
        return;

    uint32 width = x86_boot_u16(params, 0x12);
    uint32 height = x86_boot_u16(params, 0x14);
    uint32 bpp = x86_boot_u16(params, 0x16);
    uint32 pitch = x86_boot_u16(params, 0x24);
    uint64 base = x86_boot_u32(params, 0x18);
    uint64 size = x86_boot_u32(params, 0x1c);
    uint32 capabilities = x86_boot_u32(params, 0x36);

    if (capabilities & VIDEO_CAPABILITY_64BIT_BASE)
        base |= (uint64)x86_boot_u32(params, 0x3a) << 32;
    if (video_type == VIDEO_TYPE_VLFB)
        size <<= 16;

    if (base == 0 || width < 320 || height < 200 || bpp != 32 ||
        pitch < width * 4)
        return;

    uint64 min_size = (uint64)pitch * height;
    if (size < min_size)
        size = min_size;
    if (base + size <= base || base + size > X86_MAX_PROBED_PA)
        return;

    platform.framebuffer_base = base;
    platform.framebuffer_size = size;
    platform.framebuffer_width = width;
    platform.framebuffer_height = height;
    platform.framebuffer_pitch = pitch;
    platform.framebuffer_bpp = bpp;
    platform.framebuffer_red_pos = x86_boot_u8(params, 0x27);
    platform.framebuffer_green_pos = x86_boot_u8(params, 0x29);
    platform.framebuffer_blue_pos = x86_boot_u8(params, 0x2b);
    platform.has_framebuffer = 1;

    if (platform.reserved_count < MAX_RESERVED_REGIONS) {
        platform.reserved[platform.reserved_count].base = base;
        platform.reserved[platform.reserved_count].size = size;
        platform.reserved_count++;
    }
}

static void x86_consider_early_mem_end(uint64 region_base, uint64 region_end,
                                       uint64 *chosen_end)
{
    if (chosen_end == 0 || region_end <= 0x100000ULL ||
        region_base >= X86_EARLY_MEM_LIMIT)
        return;

    if (region_end > X86_EARLY_MEM_LIMIT)
        region_end = X86_EARLY_MEM_LIMIT;
    if (region_end > *chosen_end)
        *chosen_end = region_end;
}

static int x86_parse_linux_boot_params(void *boot_params, uint64 *base_out,
                                       uint64 *size_out)
{
    uint8 *params = (uint8 *)boot_params;

    if (params == 0 || x86_boot_u32(params, 0x202) != 0x53726448U)
        return -1;

    memset(&platform, 0, sizeof(platform));
    platform.reserved = x86_reserved_regions;

    uint8 entries = x86_boot_u8(params, 0x1e8);
    if (entries == 0 || entries > 128)
        return -1;

    struct x86_linux_e820_entry *e820 =
        (struct x86_linux_e820_entry *)(params + 0x2d0);
    uint64 chosen_base = 0x100000ULL;
    uint64 chosen_end = 0;

    for (uint32 i = 0; i < entries; i++) {
        uint64 region_base = e820[i].addr;
        uint64 region_end  = region_base + e820[i].size;

        if (e820[i].size == 0 || region_end < region_base ||
            region_base >= X86_MAX_PROBED_PA || region_end > X86_MAX_PROBED_PA)
            continue;

        if (e820[i].type == 1) {
            if (region_end > 0x100000ULL &&
                platform.mem_count < MAX_MEM_REGIONS) {
                if (region_base < 0x100000ULL)
                    region_base = 0x100000ULL;
                if (region_base < region_end) {
                    platform.mem[platform.mem_count].base = region_base;
                    platform.mem[platform.mem_count].size =
                        region_end - region_base;
                    platform.total_mem += region_end - region_base;
                    x86_consider_early_mem_end(region_base, region_end,
                                               &chosen_end);
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

    uint64 cmdline_ptr = x86_boot_u32(params, 0x228);
    if (x86_boot_u16(params, 0x206) >= 0x0202)
        cmdline_ptr |= (uint64)x86_boot_u32(params, 0x0c8) << 32;
    if (cmdline_ptr != 0 && cmdline_ptr < (4ULL << 30)) {
        const char *cmdline = (const char *)cmdline_ptr;
        size_t len = strnlen(cmdline, CMDLINE_MAX - 1);
        if (len > 0) {
            memcpy(platform.cmdline, cmdline, len);
            platform.cmdline[len] = '\0';
            platform.has_cmdline = 1;
        }
    }

    uint64 ramdisk_base = x86_boot_u32(params, 0x218);
    uint64 ramdisk_size = x86_boot_u32(params, 0x21c);
    if (x86_boot_u16(params, 0x206) >= 0x0203) {
        ramdisk_base |= (uint64)x86_boot_u32(params, 0x0c0) << 32;
        ramdisk_size |= (uint64)x86_boot_u32(params, 0x0c4) << 32;
    }
    if (ramdisk_base != 0 && ramdisk_size != 0 &&
        ramdisk_base + ramdisk_size > ramdisk_base &&
        ramdisk_base + ramdisk_size <= X86_MAX_PROBED_PA) {
        platform.ramdisk_base = ramdisk_base;
        platform.ramdisk_size = ramdisk_size;
        platform.has_ramdisk = 1;
        if (platform.reserved_count < MAX_RESERVED_REGIONS) {
            platform.reserved[platform.reserved_count].base = ramdisk_base;
            platform.reserved[platform.reserved_count].size = ramdisk_size;
            platform.reserved_count++;
        }
    }

    x86_parse_linux_framebuffer(params);

    if (platform.mem_count == 0 || chosen_end <= chosen_base)
        return -1;

    x86_mem_source = "linux-e820";
    platform.ncpu = 1;
    *base_out = chosen_base;
    *size_out = chosen_end - chosen_base;
    return 0;
}

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
    uint64 chosen_base = 0x100000ULL;
    uint64 chosen_end = 0;

    for (uint32 i = 0; i < si->memmap_entries; i++) {
        uint64 region_base = entries[i].addr;
        uint64 region_end  = region_base + entries[i].size;

        if (entries[i].size == 0 || region_end < region_base ||
            region_base >= X86_MAX_PROBED_PA || region_end > X86_MAX_PROBED_PA)
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
                    x86_consider_early_mem_end(region_base, region_end,
                                               &chosen_end);
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

    if (platform.mem_count == 0 || chosen_end <= chosen_base)
        return -1;

    x86_mem_source = "pvh";
    platform.ncpu = 1;
    *base_out = chosen_base;
    *size_out = chosen_end - chosen_base;
    return 0;
}

static int x86_parse_bootloader_memory(void *boot_params, uint64 *base_out,
                                       uint64 *size_out)
{
    if (boot_params == 0 || base_out == 0 || size_out == 0)
        return -1;

    if (x86_parse_linux_boot_params(boot_params, base_out, size_out) == 0)
        return 0;

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
    uint64 chosen_base = 0x100000ULL;
    uint64 chosen_end = 0;

    while (mmap_ptr < mmap_end) {
        struct x86_multiboot_mmap_entry *entry =
            (struct x86_multiboot_mmap_entry *)mmap_ptr;
        uint64 region_base = entry->addr;
        uint64 region_end  = region_base + entry->len;

        if (entry->len != 0 && region_end >= region_base &&
            region_base < X86_MAX_PROBED_PA && region_end <= X86_MAX_PROBED_PA) {
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
                        x86_consider_early_mem_end(region_base, region_end,
                                                   &chosen_end);
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

    if (platform.mem_count == 0 || chosen_end <= chosen_base)
        return -1;

    x86_mem_source = "multiboot";
    platform.ncpu = 1;
    *base_out = chosen_base;
    *size_out = chosen_end - chosen_base;
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
        x86_mem_source       = "fallback";
    }

    uint64 first_base = platform.mem[0].base;
    uint64 first_end  = platform.mem[0].base + platform.mem[0].size;
    printf("x86 boot memory: source=%s regions=%d reserved=%d total=%ld MB "
           "first=[0x%lx-0x%lx)\n",
           x86_mem_source, platform.mem_count, platform.reserved_count,
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

    if (platform.has_framebuffer) {
        printf("x86 framebuffer: 0x%lx - 0x%lx %ux%ux%u pitch=%u rgb=%u/%u/%u\n",
               platform.framebuffer_base,
               platform.framebuffer_base + platform.framebuffer_size,
               platform.framebuffer_width, platform.framebuffer_height,
               platform.framebuffer_bpp, platform.framebuffer_pitch,
               platform.framebuffer_red_pos, platform.framebuffer_green_pos,
               platform.framebuffer_blue_pos);
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

    x86_normalize_mem_regions();
    if (platform.mem_count == 0)
        return;

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
    x86_normalize_mem_regions();

    if (platform.mem_count > 0 && platform.mem[0].size > 0) {
        __physical_memory_start = platform.mem[0].base;

        uint64 highest_end = platform.mem[0].base + platform.mem[0].size;
        for (int i = 1; i < platform.mem_count; i++) {
            uint64 region_end = platform.mem[i].base + platform.mem[i].size;
            if (region_end > highest_end)
                highest_end = region_end;
        }
        /*
         * The boot-time x86 page tables only cover the low 4 GiB.  Hyper-V's
         * EFI memory map can contain usable regions above that boundary before
         * the final kernel page table exists; extending the early allocator to
         * those addresses lets pre-VM init allocations land in unmapped memory.
         * Keep the early physical span bounded to the aperture that is mapped
         * now.  The individual highmem entries remain in platform.mem[] for
         * later reservation/probing work.
         */
        if (highest_end > X86_EARLY_MEM_LIMIT)
            highest_end = X86_EARLY_MEM_LIMIT;
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
        printf("x86 boot memory: source=%s regions=%d reserved=%d total=%ld MB first=[0x%lx-0x%lx)\n",
               x86_mem_source, platform.mem_count, platform.reserved_count,
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

    if (platform.has_framebuffer) {
        printf("x86 framebuffer: [0x%lx-0x%lx) %ux%ux%u pitch=%u\n",
               platform.framebuffer_base,
               platform.framebuffer_base + platform.framebuffer_size,
               platform.framebuffer_width, platform.framebuffer_height,
               platform.framebuffer_bpp, platform.framebuffer_pitch);
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
    x86_prepare_cpu_apic_ids(cpu_limit);

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
    uint32 *runtime_ap_boot_cpu_limit =
        (uint32 *)(AP_BOOT_PA +
                   ((uint64)&ap_boot_cpu_limit -
                    (uint64)ap_trampoline_start));
    uint32 *runtime_ap_boot_next_cpu =
        (uint32 *)(AP_BOOT_PA +
                   ((uint64)&ap_boot_next_cpu -
                    (uint64)ap_trampoline_start));

    printf("[SMP] Starting secondary CPUs (prepared=%d, trampoline at 0x%x, size %d)\n",
           cpu_limit, AP_BOOT_PA, (int)tramp_size);

    /*
     * INIT-SIPI-SIPI sequence.  Start APs one at a time so the trampoline's
     * claimed hartid matches x86_cpu_apic_ids[hartid].  Hyper-V commonly
     * exposes sparse LAPIC IDs (for example 0,2,4,...), so CPU index and APIC
     * destination cannot be treated as the same number.
     */
    uint32 claimed_cpus = 1;
    for (int cpu = 1; cpu < cpu_limit; cpu++) {
        int apicid = x86_cpu_apic_id(cpu);
        if (apicid < 0 || apicid > 255) {
            printf("[SMP] CPU %d LAPIC ID %d needs x2APIC startup; stopping at %d CPUs\n",
                   cpu, apicid, claimed_cpus);
            break;
        }

        __atomic_store_n(runtime_ap_boot_cpu_limit, (uint32)(cpu + 1),
                         __ATOMIC_RELEASE);
        __atomic_store_n(runtime_ap_boot_next_cpu, (uint32)cpu,
                         __ATOMIC_RELEASE);

        while (lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_STATUS)
            ;
        lapic_write(LAPIC_ICR_HI, (uint32)apicid << 24);
        lapic_write(LAPIC_ICR_LO,
                    LAPIC_ICR_INIT | LAPIC_ICR_LEVEL |
                    LAPIC_ICR_DEST_NONE);

        /* Wait ~10 ms (busy-loop using port 0x80 delay, ~1us each) */
        for (volatile int i = 0; i < 10000; i++)
            asm volatile("outb %%al, $0x80" : : "a"(0));

        /* Send STARTUP IPI twice */
        for (int sipi = 0; sipi < 2; sipi++) {
            while (lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_STATUS)
                ;
            lapic_write(LAPIC_ICR_HI, (uint32)apicid << 24);
            lapic_write(LAPIC_ICR_LO,
                        LAPIC_ICR_STARTUP |
                        (AP_BOOT_PA >> 12) |
                        LAPIC_ICR_DEST_NONE);

            /* Wait ~200 us */
            for (volatile int i = 0; i < 200; i++)
                asm volatile("outb %%al, $0x80" : : "a"(0));
        }

        uint32 next = cpu;
        for (volatile int i = 0; i < 500000; i++) {
            next = __atomic_load_n(runtime_ap_boot_next_cpu,
                                   __ATOMIC_ACQUIRE);
            if (next > (uint32)cpu)
                break;
            asm volatile("pause");
        }
        if (next <= (uint32)cpu) {
            printf("[SMP] CPU %d LAPIC ID %d did not start; stopping at %d CPUs\n",
                   cpu, apicid, claimed_cpus);
            break;
        }
        claimed_cpus = next;
    }

    /* ap_boot_next_cpu started at 1 (BSP=0). After APs boot it equals
     * the total number of CPUs (BSP + APs). */
    uint32 final_claimed =
        __atomic_load_n(runtime_ap_boot_next_cpu, __ATOMIC_ACQUIRE);
    if (final_claimed > claimed_cpus)
        claimed_cpus = final_claimed;
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

void platform_visual_checkpoint(uint32 color)
{
    if (!platform.has_framebuffer ||
        platform.framebuffer_bpp != 32 ||
        platform.framebuffer_width == 0 ||
        platform.framebuffer_height == 0 ||
        platform.framebuffer_pitch < platform.framebuffer_width * 4)
        return;

    uint64 size = (uint64)platform.framebuffer_pitch *
                  platform.framebuffer_height;
    if (size == 0 || size > platform.framebuffer_size)
        return;

    volatile uint8 *fb = (volatile uint8 *)PA2VA(platform.framebuffer_base);
    for (uint32 y = 0; y < platform.framebuffer_height; y++) {
        volatile uint32 *row =
            (volatile uint32 *)(fb + (uint64)y * platform.framebuffer_pitch);
        for (uint32 x = 0; x < platform.framebuffer_width; x++)
            row[x] = color;
    }
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
