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
#include "x86.h"
#include "dev/fdt.h"     /* platform_info */
#include "defs.h"
#include "printf.h"
#include "memlayout.h"
#include "seg.h"
#include <smp/percpu_types.h>

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

/*
 * Trap-entry aliases: keep separate from TRAMPOLINE_CPULOCAL so that
 * TRAMPOLINE_CPULOCAL can be dedicated to shared per-CPU data (mycpu).
 */

extern void vector0(void);
extern void syscall_entry(void);
extern char trampoline[];
extern void sig_trampoline(void);
extern struct cpu_local cpus[NCPU];
extern pagetable_t kernel_pagetable;
extern uint64 trampoline_ksatp;  /* defined in trapvec.S */

static uint8 irq_stacks[NCPU][INTR_STACK_SIZE] __attribute__((aligned(PGSIZE)));

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

    /* Map APIC & PCI device MMIO region.
     * I/O APIC at 0xFEC00000, LAPIC at 0xFEE00000, HPET at 0xFED00000.
     * PCI device BARs typically assigned in 0xFEB00000-0xFEBFFFFF range.
     * Use uncacheable semantics (no PTE_G) for device MMIO. */
    uint64 flags_mmio = X86_PTE_P | X86_PTE_W;
    kvm_map_2m_range(0xFE000000ULL, 0xFF000000ULL, flags_mmio);
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
    kernel_pagetable = (pagetable_t)kpml4;

    /*
    * Map trap-entry text (vector stubs + alltraps + syscall_entry +
    * trampoline_ksatp/trampoline_kstack data) into the shared high
    * virtual window under PX(3, TRAMPOLINE).
     *
     * Compute the number of pages needed dynamically from the symbol
     * range [vector0 .. trapvec_end).
     */
    {
        extern char trapvec_end[];
        uint64 trapvec_page0 = PGROUNDDOWN((uint64)vector0);
        uint64 trapvec_last  = PGROUNDDOWN((uint64)trapvec_end);
        int npages = (trapvec_last - trapvec_page0) / PGSIZE + 1;

        for (int i = 0; i < npages; i++) {
            if (mappages((pagetable_t)kpml4,
                         TRAPVEC_ALIAS_BASE + i * PGSIZE, PGSIZE,
                         trapvec_page0 + i * PGSIZE, PTE_R | PTE_W) != 0)
                panic("arch_vm_init: trapvec alias map page%d failed", i);
        }

        if (mappages((pagetable_t)kpml4, TRAMPOLINE, PGSIZE,
                 PGROUNDDOWN((uint64)trampoline), PTE_R) != 0)
            panic("arch_vm_init: trampoline map failed");

        /*
         * Store kernel CR3 in trampoline_ksatp (defined in trapvec.S).
         * Assembly accesses it RIP-relative from the high alias;
         * C code accesses through the identity-mapped address.
         */
        trampoline_ksatp = (uint64)kpml4;

        /*
         * Signal trampoline in PML4[510] — a separate shared PML4 entry.
         * walk() sets WALK_INTERMEDIATE_FLAGS (PTE_U|PTE_W) on intermediates,
         * so no manual PTE_U patching is needed.
         */
        if (mappages((pagetable_t)kpml4, SIG_TRAMPOLINE, PGSIZE,
                     PGROUNDDOWN((uint64)sig_trampoline),
                     PTE_R | PTE_U) != 0)
            panic("arch_vm_init: sig trampoline map failed");

        uint64 cpus_base = PGROUNDDOWN((uint64)cpus);
        if (PGROUNDUP((uint64)cpus + sizeof(cpus)) - cpus_base > PGSIZE)
            panic("arch_vm_init: cpus[] exceeds one page");
        if (mappages((pagetable_t)kpml4, TRAMPOLINE_CPULOCAL, PGSIZE,
                     cpus_base, PTE_R | PTE_W) != 0)
            panic("arch_vm_init: cpu-local map failed");

        for (int i = 0; i < NCPU; i++) {
            uint64 stack_pa = (uint64)&irq_stacks[i][0];
            uint64 stack_va = KIRQSTACK(i);
            if (mappages((pagetable_t)kpml4, stack_va, INTR_STACK_SIZE,
                         stack_pa, PTE_R | PTE_W) != 0)
                panic("arch_vm_init: irq stack map failed");

            cpus[i].intr_stacks = (void **)stack_va;
            cpus[i].intr_sp = stack_va + INTR_STACK_SIZE;
        }

        /*
         * CPU entry area: map GDT+TSS and IDT at high-canonical addresses
         * so they are accessible when running under user CR3.
         * iretq reads the GDT to validate CS/SS; the IDT/TSS must be
         * reachable for fault delivery during the transition.
         */
        if (mappages((pagetable_t)kpml4, CPU_ENTRY_GDT, PGSIZE,
                     x86_gdt_page_pa(), PTE_R | PTE_W) != 0)
            panic("arch_vm_init: gdt page map failed");
        if (mappages((pagetable_t)kpml4, CPU_ENTRY_IDT, PGSIZE,
                     x86_idt_page_pa(), PTE_R) != 0)
            panic("arch_vm_init: idt page map failed");
    }

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
    return mappages(pagetable, va, size, pa, perm);
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
#include <smp/percpu.h>
#include <smp/ipi.h>
#include "string.h"
#include "errno.h"
#include "proc/thread.h"
#include <mm/memlayout.h>

pagetable_t kernel_pagetable;

/* ── TLB management (x86_64: IPI-based TLB shootdown) ── */

void vm_remote_sfence(vm_t *vm)
{
    push_off();
    smp_mb();

    cpumask_t cpumask = smp_load_acquire(&vm->cpumask);
    cpumask &= ~(1ULL << cpuid());

    if (cpumask)
        ipi_send_mask(cpumask, 0, IPI_REASON_TLB_FLUSH);

    pop_off();
}

void vm_remote_fence_i(vm_t *vm)
{
    /* x86 has coherent I-caches; a TLB flush suffices. */
    vm_remote_sfence(vm);
}

/* ── User page-table creation / destruction ── */

pagetable_t uvmcreate(void)
{
    pagetable_t pagetable = (pagetable_t)pgtab_alloc();
    if (pagetable == 0)
        return 0;
    __builtin_memset(pagetable, 0, PGSIZE);

    /*
     * Share the PML4 entries that contain high-canonical mappings:
     *   PML4[511] — TRAMPOLINE, TRAMPOLINE_DATA, CPU_ENTRY_AREA, etc.
     *   PML4[510] — SIG_TRAMPOLINE (signal return trampoline, PTE_U).
     *
     * Do not copy PML4[0] (kernel identity map), so user page tables
     * never reach identity-mapped kernel low addresses.
     */
    pagetable[PX(3, TRAMPOLINE)]     = kpml4[PX(3, TRAMPOLINE)];
    pagetable[PX(3, SIG_TRAMPOLINE)] = kpml4[PX(3, SIG_TRAMPOLINE)];

    return pagetable;
}

/*
 * Recursively free page-table pages.
 * All leaf mappings must already have been removed.
 *
 * On x86_64, PTE_V == PTE_R (both are bit 0), so we cannot distinguish
 * leaf from non-leaf entries using R/W/X flags as RISC-V does.
 * Instead we track the page-table level:
 *   level 3 = PML4, 2 = PDPT, 1 = PD — entries are non-leaf (ignoring large pages)
 *   level 0 = PT — entries are leaf
 *
 * skip_idx: PML4 index to skip (shared kernel mapping, e.g. PML4[511]).
 */
static void __freewalk(pagetable_t pagetable, int level, int skip_idx)
{
    for (int i = 0; i < 512; i++) {
        if (i == skip_idx)
            continue;
        pte_t pte = pagetable[i];
        if (!(pte & PTE_V))
            continue;
        if (level > 0) {
            /* Non-leaf: recurse into next level page table. */
            uint64 child = PTE2PA(pte);
            __freewalk((pagetable_t)child, level - 1, -1);
            pagetable[i] = 0;
        } else {
            /* Level 0 (PT): this is a leaf — should have been cleared. */
            panic("freewalk: leaf");
        }
    }
    pgtab_free((void *)pagetable);
}

void freewalk(pagetable_t pagetable)
{
    __freewalk(pagetable, 3, PX(3, TRAMPOLINE));
}

void uvmfree(pagetable_t pagetable, uint64 sz)
{
    (void)sz;
    if (pagetable != 0) {
        /* Clear shared kernel PML4 entries before freewalk so the
         * kernel page tables are not accidentally freed. */
        pagetable[PX(3, TRAMPOLINE)] = 0;
        pagetable[PX(3, SIG_TRAMPOLINE)] = 0;
    }
    freewalk(pagetable);
}

/* ── Trampoline / trapframe mapping ── */

/*
 * arch_vm_setup_trampoline — set up the trapframe PTE tracking
 * for a new process VM.
 *
 * Walks to the first TRAPFRAME VA to create intermediate page tables,
 * then stores the leaf page table pointer in vm->trapframe_pte.
 * All 64 per-CPU trapframe pages share the same leaf page table
 * (they're within a single 2MB region).
 */
int arch_vm_setup_trampoline(vm_t *vm)
{
    if (vm == NULL || vm->pagetable == NULL)
        return -1;

    /* Walk to TRAPFRAME VA, alloc=1 to create intermediate page tables. */
    pte_t *pte = walk(vm->pagetable, TRAPFRAME, 1, NULL, NULL);
    if (pte == NULL)
        return -1;

    /* Store the leaf page table base (the page containing this PTE). */
    vm->trapframe_pte = (pte_t *)PGROUNDDOWN((uint64)pte);
    return 0;
}

void arch_vm_teardown_trampoline(vm_t *vm)
{
    if (vm == NULL || vm->pagetable == NULL || vm->trapframe_pte == NULL)
        return;
    /* Clear all trapframe PTEs (don't free phys pages — they belong
     * to kernel stacks, not to this VM). */
    for (int i = 0; i < NCPU; i++) {
        int pte_idx = PX(0, TRAPFRAME + (i * PGSIZE));
        vm->trapframe_pte[pte_idx] = 0;
    }
}

/* ── Per-CPU trapframe PTE management ── */

/*
 * vm_cpu_online — mark a CPU as using this VM and map the current
 *                 thread's trapframe page into the user page table.
 *
 * Called from usertrapret() before returning to user mode.
 * Returns the utrapframe virtual address (user VA) for this CPU.
 */
uint64 vm_cpu_online(vm_t *vm, int cpu, struct thread *p)
{
    uint64 trapframe_poffset = TRAPFRAME_POFFSET;

    if (vm->trapframe_pte != NULL && p != NULL && p->trapframe != NULL) {
        int pte_idx = PX(0, TRAPFRAME + (cpu * PGSIZE));
        uint64 trapframe_pa = PGROUNDDOWN((uint64)p->trapframe);
        vm->trapframe_pte[pte_idx] =
            PA2PTE(trapframe_pa) | PTE_R | PTE_W | PTE_V | PTE_A | PTE_D;
        /* Ensure PTE write is visible before page table switch. */
        asm volatile("" ::: "memory");
        /* Compute actual offset of utrapframe within the page. */
        trapframe_poffset = (uint64)p->trapframe & (PGSIZE - 1);
    }
    atomic_or(&vm->cpumask, (1ULL << cpu));
    return TRAPFRAME + (cpu * PGSIZE) + trapframe_poffset;
}

void vm_cpu_offline(vm_t *vm, int cpu)
{
    atomic_and(&vm->cpumask, ~(1ULL << cpu));
}

cpumask_t vm_get_cpumask(vm_t *vm)
{
    return vm->cpumask;
}

/* ── Page-table dump (x86_64 4-level format — minimal stub) ── */

void dump_pagetable(pagetable_t pagetable, int level, int indent,
                    uint64 va_base, uint64 va_end, bool omit_pa)
{
    (void)pagetable; (void)level; (void)indent;
    (void)va_base; (void)va_end; (void)omit_pa;
    printf("dump_pagetable: not yet implemented for x86_64\n");
}