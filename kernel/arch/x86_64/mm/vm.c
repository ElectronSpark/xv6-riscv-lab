/**
 * @file vm.c
 * @brief x86_64 kernel page table setup.
 *
 * Builds 4-level (PML4 -> PDPT -> PD) identity-mapped page tables
 * covering all usable physical memory detected by the bootloader.
 * Uses 2 MiB large pages for efficiency.
 *
 * The boot assembly (entry.S) already sets up a minimal identity map of the
 * first 1 GiB using 2 MiB pages.  arch_vm_init() replaces that with proper
 * tables that cover the full detected RAM.
 */

#include "types.h"
#include "arch/vm.h"
#include "dev/fdt.h"     /* platform_info */
#include "defs.h"
#include "printf.h"

/* ── x86_64 page table entry bit definitions ── */
#define X86_PTE_P       (1ULL << 0)     /* Present */
#define X86_PTE_W       (1ULL << 1)     /* Writable */
#define X86_PTE_U       (1ULL << 2)     /* User-accessible */
#define X86_PTE_PS      (1ULL << 7)     /* Page size (2M in PD) */
#define X86_PTE_G       (1ULL << 8)     /* Global */

#define X86_PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

#define X86_PAGE_SIZE   4096
#define X86_LARGE_PAGE  (2ULL * 1024 * 1024)    /* 2 MiB */

/* Max 512 entries per table level */
#define X86_PTE_PER_TABLE 512

/* PML4/PDPT/PD index from virtual address */
#define PDPT_IDX(va) (((va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((va) >> 21) & 0x1FF)

/* ── Static page table storage ──
 * Enough tables to map up to 4 GiB with 2 MiB large pages:
 *   1 PML4 + 1 PDPT + 4 PDs = 6 pages
 */
#define MAX_PDS 4

static uint64 kpml4[X86_PTE_PER_TABLE] __attribute__((aligned(X86_PAGE_SIZE)));
static uint64 kpdpt[X86_PTE_PER_TABLE] __attribute__((aligned(X86_PAGE_SIZE)));
static uint64 kpds[MAX_PDS][X86_PTE_PER_TABLE] __attribute__((aligned(X86_PAGE_SIZE)));
static int n_pds_used;

/* ── debugcon helpers ── */
static inline void vm_outb(uint16 port, uint8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static void vm_debug_puts(const char *s) {
    while (*s) vm_outb(0xE9, (uint8)*s++);
}
static void vm_debug_hex(uint64 v) {
    static const char hex[] = "0123456789abcdef";
    if (v == 0) { vm_outb(0xE9, '0'); return; }
    int started = 0;
    for (int i = 15; i >= 0; i--) {
        int nib = (v >> (i * 4)) & 0xF;
        if (nib || started) { vm_outb(0xE9, (uint8)hex[nib]); started = 1; }
    }
}

/*
 * Map a physical range using 2 MiB large pages (identity mapped: va == pa).
 */
static void kvm_map_2m_range(uint64 pa_start, uint64 pa_end, uint64 flags)
{
    pa_start &= ~(X86_LARGE_PAGE - 1);

    for (uint64 pa = pa_start; pa < pa_end; pa += X86_LARGE_PAGE) {
        int pidx = PDPT_IDX(pa);

        if (!(kpdpt[pidx] & X86_PTE_P)) {
            if (n_pds_used >= MAX_PDS) {
                vm_debug_puts("[vm] ERROR: out of PD tables\n");
                return;
            }
            __builtin_memset(kpds[n_pds_used], 0, X86_PAGE_SIZE);
            kpdpt[pidx] = (uint64)kpds[n_pds_used] | X86_PTE_P | X86_PTE_W;
            n_pds_used++;
        }

        uint64 *pd = (uint64 *)(kpdpt[pidx] & X86_PTE_ADDR_MASK);
        pd[PD_IDX(pa)] = pa | flags | X86_PTE_PS;
    }
}

static void kvm_build(void)
{
    __builtin_memset(kpml4, 0, X86_PAGE_SIZE);
    __builtin_memset(kpdpt, 0, X86_PAGE_SIZE);
    n_pds_used = 0;

    /* PML4[0] -> PDPT (covers first 512 GiB) */
    kpml4[0] = (uint64)kpdpt | X86_PTE_P | X86_PTE_W;

    uint64 flags_rw = X86_PTE_P | X86_PTE_W | X86_PTE_G;

    /* Always map the first 2 MiB (BIOS area + kernel image at 1 MiB) */
    kvm_map_2m_range(0, X86_LARGE_PAGE, flags_rw);

    /* Map each usable memory region from the bootloader */
    for (int i = 0; i < platform.mem_count; i++) {
        uint64 base = platform.mem[i].base;
        uint64 end  = base + platform.mem[i].size;
        kvm_map_2m_range(base, end, flags_rw);
    }

    /* Map reserved regions in the low 4 GiB (BIOS, ACPI tables, etc.) */
    for (int i = 0; i < platform.reserved_count; i++) {
        uint64 base = platform.reserved[i].base;
        uint64 end  = base + platform.reserved[i].size;
        if (base < (4ULL << 30)) {
            if (end > (4ULL << 30)) end = (4ULL << 30);
            kvm_map_2m_range(base, end, flags_rw);
        }
    }

    /* Map APIC MMIO region (I/O APIC at 0xFEC00000, LAPIC at 0xFEE00000).
     * Use uncacheable semantics (no PTE_G) for device MMIO. */
    uint64 flags_mmio = X86_PTE_P | X86_PTE_W;
    kvm_map_2m_range(0xFEC00000ULL, 0xFF000000ULL, flags_mmio);
}

static void kvm_load(void)
{
    asm volatile("movq %0, %%cr3" : : "r"((uint64)kpml4) : "memory");
}

void arch_vm_init(void)
{
    vm_slab_init();
    vm_debug_puts("[xv6 x86_64] arch_vm_init: building kernel page tables\n");
    kvm_build();

    vm_debug_puts("[xv6 x86_64] arch_vm_init: PDs used=");
    vm_debug_hex((uint64)n_pds_used);
    vm_debug_puts(", loading CR3=0x");
    vm_debug_hex((uint64)kpml4);
    vm_debug_puts("\n");

    kvm_load();
    vm_debug_puts("[xv6 x86_64] arch_vm_init: page tables active\n");
}

void arch_vm_init_hart(void)
{
    kvm_load();
}

int arch_vm_map(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
    (void)pagetable; (void)va; (void)size; (void)pa; (void)perm;
    return 0;
}

void arch_tlb_flush(void)
{
    uint64 cr3;
    asm volatile("movq %%cr3, %0" : "=r"(cr3));
    asm volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ==========================================================================
 * Architecture-specific VM interface stubs.
 *
 * Generic process VM code (VMA management, mmap, copyout/copyin, etc.)
 * now lives in kernel/mm/vm.c.  This file only provides the arch hooks
 * that the shared code calls.
 * ========================================================================== */

#include <mm/vm.h>
#include <mm/page.h>
#include <smp/atomic.h>
#include "string.h"
#include "errno.h"

pagetable_t kernel_pagetable;

/* ── TLB management (x86_64: no remote fences yet) ── */

void vm_remote_sfence(vm_t *vm)  { (void)vm; }
void vm_remote_fence_i(vm_t *vm) { (void)vm; }

/* ── User page-table creation / destruction ── */

pagetable_t uvmcreate(void)
{
    pagetable_t pagetable = (pagetable_t)pgtab_alloc();
    if (pagetable == 0)
        return 0;
    __builtin_memset(pagetable, 0, PGSIZE);
    return pagetable;
}

void freewalk(pagetable_t pagetable)
{
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            uint64 child = PTE2PA(pte);
            freewalk((pagetable_t)child);
            pagetable[i] = 0;
        } else if (pte & PTE_V) {
            panic("freewalk: leaf");
        }
    }
    pgtab_free((void *)pagetable);
}

void uvmfree(pagetable_t pagetable, uint64 sz)
{
    (void)sz;
    freewalk(pagetable);
}

/* ── Trampoline / trapframe (not yet implemented on x86_64) ── */

int arch_vm_setup_trampoline(vm_t *vm)
{
    (void)vm;
    return 0;
}

void arch_vm_teardown_trampoline(vm_t *vm) { (void)vm; }

/* ── Per-CPU trapframe PTE management (stub) ── */

uint64 vm_cpu_online(vm_t *vm, int cpu)
{
    (void)vm; (void)cpu;
    return 0;
}

void vm_cpu_offline(vm_t *vm, int cpu)
{
    (void)vm; (void)cpu;
}

cpumask_t vm_get_cpumask(vm_t *vm)
{
    (void)vm;
    return 0;
}

/* ── Page-table dump (x86_64 4-level format — minimal stub) ── */

void dump_pagetable(pagetable_t pagetable, int level, int indent,
                    uint64 va_base, uint64 va_end, bool omit_pa)
{
    (void)pagetable; (void)level; (void)indent;
    (void)va_base; (void)va_end; (void)omit_pa;
    printf("dump_pagetable: not yet implemented for x86_64\n");
}