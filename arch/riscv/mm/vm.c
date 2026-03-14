/**
 * @file vm.c
 * @brief RISC-V architecture-specific virtual memory code.
 *
 * Contains:
 *  - Kernel page-table setup (kvmmake) for Sv39.
 *  - arch_vm_init / arch_vm_init_hart (satp, sfence.vma).
 *  - TLB shootdown via SBI (vm_remote_sfence / vm_remote_fence_i).
 *  - User page-table creation with shared trampoline PTE (uvmcreate).
 *  - Page-table page deallocation (freewalk / uvmfree).
 *  - Trampoline / trapframe page-table setup.
 *  - Per-CPU trapframe PTE management (vm_cpu_online / offline).
 *  - Page-table dumping helpers.
 *
 * Generic process VM code (VMA management, demand paging, mmap, etc.) lives
 * in kernel/mm/vm.c.
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "arch/vm.h"
#include <mm/vm.h>
#include <mm/memlayout.h>
#include <mm/page.h>
#include <smp/percpu.h>
#include <smp/atomic.h>
#include "printf.h"
#include "proc/thread.h"
#include "string.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "dev/pci.h"
#include "timer/goldfish_rtc.h"
#include "dev/e1000_dev.h"
#include "dev/plic.h"
#include "dev/fdt.h"
#include "smp/ipi.h"
#include "sbi.h"
#include "errno.h"

/* ========================================================================== */
/*  Globals and linker symbols                                                */
/* ========================================================================== */

pagetable_t kernel_pagetable;

extern char etext[], _entry[], _init_data[], _entry_end[], _text[];
extern char _rodata[], _rodata_end[];
extern char _data[], _data_end[];
extern char _bss[], _bss_end[];
extern char trampoline[], _trampoline_data[];
extern char sig_trampoline[];
extern char _data_ktlb[];
extern uint64 trampoline_ksatp;

/* ========================================================================== */
/*  TLB management (SBI-based)                                                */
/* ========================================================================== */

void vm_remote_sfence(vm_t *vm)
{
    push_off();
    smp_mb();

    cpumask_t cpumask = smp_load_acquire(&vm->cpumask);
    cpumask &= ~(1ULL << cpuid());

    if (cpumask) {
        if (vm_asid_max() > 0 && vm->asid != 0)
            sbi_remote_hfence_vma_asid(cpumask, 0, 0, 0, vm->asid);
        else
            sbi_remote_hfence_vma(cpumask, 0, 0, 0);
    }

    pop_off();
}

void vm_remote_sfence_page(vm_t *vm, uint64 va)
{
    push_off();
    smp_mb();

    cpumask_t cpumask = smp_load_acquire(&vm->cpumask);
    cpumask &= ~(1ULL << cpuid());

    if (cpumask) {
        if (vm_asid_max() > 0 && vm->asid != 0)
            sbi_remote_hfence_vma_asid(cpumask, 0, va, PGSIZE, vm->asid);
        else
            sbi_remote_hfence_vma(cpumask, 0, va, PGSIZE);
    }

    pop_off();
}

void vm_remote_sfence_range(vm_t *vm, uint64 start, uint64 size)
{
    push_off();
    smp_mb();

    cpumask_t cpumask = smp_load_acquire(&vm->cpumask);
    cpumask &= ~(1ULL << cpuid());

    if (cpumask) {
        if (vm_asid_max() > 0 && vm->asid != 0)
            sbi_remote_hfence_vma_asid(cpumask, 0, start, size, vm->asid);
        else
            sbi_remote_hfence_vma(cpumask, 0, start, size);
    }

    pop_off();
}

void vm_remote_fence_i(vm_t *vm)
{
    push_off();
    smp_mb();

    cpumask_t cpumask = smp_load_acquire(&vm->cpumask);
    cpumask |= (1ULL << cpuid());

    sbi_remote_hfence_i(cpumask, 0);

    pop_off();
}

/* ========================================================================== */
/*  Kernel page-table setup (Sv39)                                            */
/* ========================================================================== */

/*
 * Like kvmmap, but silently skip pages that are already mapped.
 */
static void kvmmap_safe(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz,
                        int perm)
{
    for (uint64 off = 0; off < sz; off += PGSIZE) {
        pte_t *pte = walk(kpgtbl, va + off, 0, NULL, NULL);
        if (pte && (*pte & PTE_V))
            continue;
        kvmmap(kpgtbl, va + off, pa + off, PGSIZE, perm);
    }
}

pagetable_t kvmmake(void)
{
    pagetable_t kpgtbl = (void *)_data_ktlb;
    memset(kpgtbl, 0, PGSIZE);

    /* UART */
    uint64 uart_page = PGROUNDDOWN(UART0);
    kvmmap(kpgtbl, uart_page, VA2PA(uart_page), PGSIZE, PTE_R | PTE_W);

    /* Goldfish RTC (QEMU only) */
    if (GOLDFISH_RTC != 0)
        kvmmap(kpgtbl, GOLDFISH_RTC, VA2PA(GOLDFISH_RTC), PGSIZE, PTE_R | PTE_W);

    /* VirtIO MMIO */
    if (platform.has_virtio && platform.virtio_count > 0) {
        for (int i = 0; i < platform.virtio_count && i < N_VIRTIO; i++) {
            if (platform.virtio_base[i] != 0) {
                uint64 vio_va = (uint64)PA2VA(platform.virtio_base[i]);
                kvmmap(kpgtbl, vio_va, platform.virtio_base[i], PGSIZE,
                       PTE_R | PTE_W);
            }
        }
    }

    /* PCIe ECAM */
    if (platform.has_pcie && PCIE_ECAM != 0) {
        for (int i = 0; i < platform.pcie_reg_count; i++) {
            uint64 base_pa = platform.pcie_reg[i].base;
            uint64 size = platform.pcie_reg[i].size;
            if (base_pa == 0 || size == 0)
                continue;
            uint64 aligned_pa = PGROUNDDOWN(base_pa);
            uint64 aligned_size = PGROUNDUP(base_pa + size) - aligned_pa;
            kvmmap_safe(kpgtbl, (uint64)PA2VA(aligned_pa), aligned_pa,
                        aligned_size, PTE_R | PTE_W);
        }
        if (platform.has_virtio && E1000_PCI_ADDR != 0)
            kvmmap(kpgtbl, E1000_PCI_ADDR, VA2PA(E1000_PCI_ADDR), 0x20000,
                   PTE_R | PTE_W);
    }

    /* PLIC */
    if (platform.plic_base != 0 && platform.plic_size != 0)
        kvmmap(kpgtbl, PLIC, VA2PA(PLIC), platform.plic_size, PTE_R | PTE_W);

    /* EMAC MMIO (SpacemiT X1) */
    if (platform.has_emac) {
        for (int i = 0; i < platform.emac_count && i < EMAC_MAX; i++) {
            if (platform.emac[i].base != 0 && platform.emac[i].size != 0) {
                uint64 base = PGROUNDDOWN(platform.emac[i].base);
                uint64 size = PGROUNDUP(platform.emac[i].base +
                                        platform.emac[i].size) - base;
                kvmmap_safe(kpgtbl, base, base, size, PTE_R | PTE_W);
            }
            if (platform.emac[i].apmu_base != 0 &&
                platform.emac[i].ctrl_reg != 0) {
                uint64 pg = PGROUNDDOWN(
                    (uint64)platform.emac[i].apmu_base +
                    platform.emac[i].ctrl_reg);
                kvmmap_safe(kpgtbl, pg, pg, PGSIZE, PTE_R | PTE_W);
            }
            if (platform.emac[i].apmu_base != 0 &&
                platform.emac[i].dline_reg != 0) {
                uint64 pg = PGROUNDDOWN(
                    (uint64)platform.emac[i].apmu_base +
                    platform.emac[i].dline_reg);
                kvmmap_safe(kpgtbl, pg, pg, PGSIZE, PTE_R | PTE_W);
            }
        }
    }

    /* I2C MMIO (SpacemiT X1) */
    if (platform.has_i2c) {
        for (int i = 0; i < platform.i2c_count && i < X1_I2C_MAX; i++) {
            if (platform.i2c[i].base != 0 && platform.i2c[i].size != 0) {
                uint64 base = PGROUNDDOWN(platform.i2c[i].base);
                uint64 size = PGROUNDUP(platform.i2c[i].base +
                                        platform.i2c[i].size) - base;
                kvmmap_safe(kpgtbl, base, base, size, PTE_R | PTE_W);
            }
        }
    }

    /* SDHCI MMIO (SpacemiT X1) */
    if (platform.has_sdhci) {
        for (int i = 0; i < platform.sdhci_count && i < SDHCI_MAX; i++) {
            if (platform.sdhci[i].base != 0 && platform.sdhci[i].size != 0) {
                uint64 base = PGROUNDDOWN(platform.sdhci[i].base);
                uint64 size = PGROUNDUP(platform.sdhci[i].base +
                                        platform.sdhci[i].size) - base;
                kvmmap_safe(kpgtbl, base, base, size, PTE_R | PTE_W);
            }
            if (platform.sdhci[i].apmu_base != 0 &&
                platform.sdhci[i].apmu_offset != 0) {
                uint64 pg = PGROUNDDOWN(
                    (uint64)platform.sdhci[i].apmu_base +
                    platform.sdhci[i].apmu_offset);
                kvmmap_safe(kpgtbl, pg, pg, PGSIZE, PTE_R | PTE_W);
            }
        }
        if (platform.sdhci_count > 0 && platform.sdhci[0].apbc_base != 0) {
            uint64 apbc_aib = (uint64)platform.sdhci[0].apbc_base + 0x3C;
            kvmmap_safe(kpgtbl, PGROUNDDOWN(apbc_aib),
                        PGROUNDDOWN(apbc_aib), PGSIZE, PTE_R | PTE_W);
        }
    }

    /* Early boot stub (_entry/_entry_end are PAs, map at higher-half VA) */
    /* .text.entry: executable code (R|X) */
    kvmmap(kpgtbl, (uint64)PA2VA(_entry), (uint64)_entry,
           PGROUNDUP((uint64)_init_data) - (uint64)_entry, PTE_R | PTE_X | PTE_G);
    /* .init.data + .init.bss (stack0): data (R|W) */
    if ((uint64)_entry_end > (uint64)_init_data)
        kvmmap(kpgtbl, (uint64)PA2VA(_init_data), (uint64)_init_data,
               PGROUNDUP((uint64)_entry_end) - PGROUNDDOWN((uint64)_init_data),
               PTE_R | PTE_W | PTE_G);

    /* Kernel text (executable, read-only) */
    kvmmap(kpgtbl, (uint64)_text, VA2PA((uint64)_text),
           (uint64)etext - (uint64)_text, PTE_R | PTE_X | PTE_G);

    /* Trampoline */
    kvmmap(kpgtbl, TRAMPOLINE, VA2PA((uint64)trampoline), PGSIZE, PTE_R | PTE_X | PTE_G);
    kvmmap(kpgtbl, TRAMPOLINE_DATA, VA2PA((uint64)_trampoline_data), PGSIZE, PTE_R | PTE_G);
    kvmmap(kpgtbl, (uint64)_trampoline_data, VA2PA((uint64)_trampoline_data), PGSIZE,
           PTE_R | PTE_W | PTE_G);
    kvmmap(kpgtbl, TRAMPOLINE_CPULOCAL, VA2PA((uint64)cpus), PGSIZE, PTE_R | PTE_W | PTE_G);
    kvmmap(kpgtbl, (uint64)cpus, VA2PA((uint64)cpus), PGSIZE, PTE_R | PTE_W | PTE_G);
    kvmmap(kpgtbl, SIG_TRAMPOLINE, VA2PA((uint64)sig_trampoline), PGSIZE,
           PTE_R | PTE_X | PTE_U | PTE_G);

    /* Read-only data */
    kvmmap(kpgtbl, (uint64)_rodata, VA2PA((uint64)_rodata),
           (uint64)_rodata_end - (uint64)_rodata, PTE_R | PTE_G);

    /* Kernel top-level page table */
    kvmmap(kpgtbl, (uint64)_data_ktlb, VA2PA((uint64)_data_ktlb), PGSIZE,
           PTE_R | PTE_W | PTE_G);

    /* Data */
    kvmmap(kpgtbl, (uint64)_data, VA2PA((uint64)_data),
           (uint64)_data_end - (uint64)_data, PTE_R | PTE_W | PTE_G);

    /* BSS */
    kvmmap(kpgtbl, (uint64)_bss, VA2PA((uint64)_bss),
           (uint64)_bss_end - (uint64)_bss, PTE_R | PTE_W | PTE_G);

    /* Physical RAM regions */
    {
        uint64 first_region_end = platform.mem[0].base + platform.mem[0].size;
        if (first_region_end > PHYSTOP)
            first_region_end = PHYSTOP;
        kvmmap(kpgtbl, (uint64)_bss_end, VA2PA((uint64)_bss_end),
               (uint64)PA2VA(first_region_end) - (uint64)_bss_end, PTE_R | PTE_W | PTE_G);

        for (int i = 1; i < platform.mem_count; i++) {
            uint64 region_base = platform.mem[i].base;
            uint64 region_end = region_base + platform.mem[i].size;
            if (region_end > PHYSTOP)
                region_end = PHYSTOP;
            if (region_base < region_end) {
                printf("mapping highmem region [%d]: 0x%lx - 0x%lx (%ld MB)\n",
                       i, region_base, region_end,
                       (region_end - region_base) / (1024 * 1024));
                kvmmap(kpgtbl, (uint64)PA2VA(region_base), region_base,
                       region_end - region_base, PTE_R | PTE_W | PTE_G);
            }
        }
    }

    /* Kernel symbols */
    if (KERNEL_SYMBOLS_SIZE > 0) {
        kvmmap(kpgtbl, KERNEL_SYMBOLS_START, VA2PA(KERNEL_SYMBOLS_START),
               KERNEL_SYMBOLS_SIZE, PTE_R | PTE_G);
    }

    /* Identity-map physical RAM so PAs from page_alloc / slab are directly
     * usable as kernel pointers, matching x86's PML4[0] identity map.
     * Use 1GB superpages (level-2 leaf entries) — supervisor-only, no PTE_G
     * so entries are flushed on SATP switch from kernel to user page table. */
    {
        unsigned id_start = ((uint64)KERNBASE >> 30) & 0x1FF;
        unsigned id_end = (((uint64)PHYSTOP - 1) >> 30) & 0x1FF;
        for (unsigned i = id_start; i <= id_end; i++) {
            if (kpgtbl[i] & PTE_V)
                continue;  /* don't overwrite an existing mapping */
            uint64 pa = (uint64)i << 30;
            kpgtbl[i] = PA2PTE(pa) | PTE_R | PTE_W | PTE_V | PTE_A | PTE_D;
        }
        printf("identity map: L2[%u..%u] VA [0x%lx, 0x%lx)\n",
               id_start, id_end,
               (uint64)id_start << 30, ((uint64)id_end + 1) << 30);
    }

    return kpgtbl;
}

/* ========================================================================== */
/*  arch_vm_init / arch_vm_init_hart                                          */
/* ========================================================================== */

void arch_vm_init(void)
{
    vm_slab_init();           /* shared slab pool init */
    kernel_pagetable = kvmmake();

    smp_store_release(&trampoline_ksatp, MAKE_SATP(VA2PA(kernel_pagetable)));
    smp_mb();

    /* Detect ASID width: write maximum ASID to SATP, read back, see what
     * sticks.  Keep the current PPN valid so memory access still works. */
    {
        uint64 old = r_satp();
        uint64 ppn = old & ((1UL << 44) - 1);
        w_satp(SATP_SV39 | SATP_ASID_MASK | ppn);
        uint64 test = r_satp();
        w_satp(old);
        sfence_vma();
        uint16 max_asid = (test >> SATP_ASID_SHIFT) & 0xFFFF;
        vm_asid_init(max_asid);
    }
}

void arch_vm_init_hart(void)
{
    sfence_vma();
    w_satp(trampoline_ksatp);
    sfence_vma();
    printf("hart %ld switched to kernel page table\n", cpuid());
}

void arch_tlb_flush(void) { sfence_vma(); }

int arch_vm_map(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
                int perm)
{
    return mappages(pagetable, va, size, pa, perm);
}

/* ========================================================================== */
/*  User page-table creation / destruction                                    */
/* ========================================================================== */

/*
 * Top-level PTE index for trampoline region.
 * This entry is shared with the kernel page table so trampoline mappings
 * are available in every address space.
 */
#define TRAMPOLINE_PTE_IDX ((TRAMPOLINE >> 30) & 0x1FF)

pagetable_t uvmcreate(void)
{
    pagetable_t kpgtbl = (pagetable_t)_data_ktlb;
    pagetable_t pagetable = (pagetable_t)pgtab_alloc();
    if (pagetable == 0)
        return 0;
    memset(pagetable, 0, PGSIZE);
    /* Share all upper-half PTEs (kernel space) and the trampoline PTE
     * from the kernel page table so that kernel space is always visible. */
    for (int i = TRAMPOLINE_PTE_IDX; i < 512; i++)
        pagetable[i] = kpgtbl[i];
    return pagetable;
}

/*
 * Recursively free page-table pages.
 * All leaf mappings must already have been removed.
 */
static void __freewalk(pagetable_t pagetable, int level)
{
    for (int i = 0; i < 512; i++) {
        /* At the top level, skip all shared entries (trampoline + kernel). */
        if (level == 2 && i >= TRAMPOLINE_PTE_IDX)
            break;
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            uint64 child = PTE2PA(pte);
            __freewalk((pagetable_t)PA2VA(child), level - 1);
            pagetable[i] = 0;
        } else if (pte & PTE_V) {
            panic("freewalk: leaf");
        }
    }
    pgtab_free((void *)pagetable);
}

void freewalk(pagetable_t pagetable)
{
    __freewalk(pagetable, 2);
}

void uvmfree(pagetable_t pagetable, uint64 sz)
{
    /* Clear shared entries (trampoline + kernel) before freeing. */
    for (int i = TRAMPOLINE_PTE_IDX; i < 512; i++)
        pagetable[i] = 0;
    freewalk(pagetable);
}

/* ========================================================================== */
/*  Trampoline / trapframe page-table management                              */
/* ========================================================================== */

int arch_vm_setup_trampoline(vm_t *vm)
{
    if (vm == NULL || vm->pagetable == NULL)
        return -EINVAL;

    /* Walk to the first trapframe VA to create intermediate page tables. */
    pte_t *pte = walk(vm->pagetable, TRAPFRAME, 1, NULL, NULL);
    if (pte == NULL)
        return -ENOMEM;

    /* Store the leaf page table base (covers 512 consecutive 4KB pages). */
    vm->trapframe_pte = (pte_t *)PGROUNDDOWN((uint64)pte);
    return 0;
}

void arch_vm_teardown_trampoline(vm_t *vm)
{
    if (vm == NULL || vm->pagetable == NULL || vm->trapframe_pte == NULL)
        return;
    /* Clear all trapframe PTEs but don't free the physical pages. */
    for (int i = 0; i < NCPU; i++) {
        int pte_idx = PX(0, TRAPFRAME + (i * PGSIZE));
        vm->trapframe_pte[pte_idx] = 0;
    }
}

/* ========================================================================== */
/*  Per-CPU trapframe PTE management                                          */
/* ========================================================================== */

uint64 vm_cpu_online(vm_t *vm, int cpu, struct thread *p)
{
    uint64 trapframe_poffset = TRAPFRAME_POFFSET;
    if (vm->trapframe_pte != NULL && p != NULL && p->trapframe != NULL) {
        int pte_idx = PX(0, TRAPFRAME + (cpu * PGSIZE));
        uint64 trapframe_pa = PGROUNDDOWN((uint64)p->trapframe);
        vm->trapframe_pte[pte_idx] =
            PA2PTE(trapframe_pa) | PTE_R | PTE_W | PTE_V | PTE_A | PTE_D;
        smp_wmb();
        trapframe_poffset = (uint64)p->trapframe & (PGSIZE - 1);
    }
    atomic_or(&vm->cpumask, (1ULL << cpu));
    return TRAPFRAME + (cpu * PGSIZE) + trapframe_poffset;
}

void vm_cpu_offline(vm_t *vm, int cpu)
{
    atomic_and(&vm->cpumask, ~(1ULL << cpu));
}

cpumask_t vm_get_cpumask(vm_t *vm) { return smp_load_acquire(&vm->cpumask); }

/* ========================================================================== */
/*  Page-table dump (RISC-V Sv39 format)                                      */
/* ========================================================================== */

void dump_pagetable(pagetable_t pagetable, int level, int indent,
                    uint64 va_base, uint64 va_end, bool omit_pa)
{
    if (level < 0 || level > 2) {
        printf("Invalid level %d for pagetable dump\n", level);
        return;
    }

    int idx_start = 0;
    int idx_end = 512;
    idx_start = PX(2, va_base);
    if (level == 2 && va_end != 0)
        idx_end = PX(2, va_end);

    if (level == 0) {
        int chunk_start = -1;
        uint64 chunk_va_start = 0;
        uint64 chunk_pa_start = 0;
        uint32 chunk_flags = 0;
        int chunk_count = 0;

        for (int i = idx_start; i <= idx_end; i++) {
            pte_t pte = (i < idx_end) ? pagetable[i] : 0;
            uint64 va = va_base | (((uint64)i) << 12);
            uint64 pa = PTE2PA(pte);
            uint32 flags = PTE_FLAGS(pte);

            bool valid_entry = (i < idx_end) && (pte & PTE_V) &&
                               !(omit_pa && va >= (uint64)PA2VA(KERNBASE) && va < (uint64)PA2VA(PHYSTOP));

            if (valid_entry && chunk_start == -1) {
                chunk_start = i;
                chunk_va_start = va;
                chunk_pa_start = pa;
                chunk_flags = flags;
                chunk_count = 1;
            } else if (valid_entry && chunk_start != -1 &&
                       pa == chunk_pa_start + (chunk_count * PGSIZE) &&
                       flags == chunk_flags) {
                chunk_count++;
            } else {
                if (chunk_start != -1) {
                    const char *str_v = (chunk_flags & PTE_V) ? "V" : " ";
                    const char *str_u = (chunk_flags & PTE_U) ? "U" : " ";
                    const char *str_w = (chunk_flags & PTE_W) ? "W" : " ";
                    const char *str_x = (chunk_flags & PTE_X) ? "X" : " ";
                    const char *str_r = (chunk_flags & PTE_R) ? "R" : " ";
                    const char *str_rsw = " ";

                    if (chunk_count == 1) {
                        printf("%*sPTE[%d](%p): %lx(%s%s%s%s%s%s), (va, pa): "
                               "(%p, %p)\n",
                               indent, "", chunk_start, &pagetable[i],
                               chunk_flags & ~PTE_V, str_v, str_u, str_w,
                               str_x, str_r, str_rsw,
                               (void *)chunk_va_start,
                               (void *)chunk_pa_start);
                    } else {
                        printf("%*sPTE[%d-%d]: %lx(%s%s%s%s%s%s), (va, pa): "
                               "(%p-%p, %p-%p) [%d pages]\n",
                               indent, "", chunk_start,
                               chunk_start + chunk_count - 1,
                               chunk_flags & ~PTE_V, str_v, str_u, str_w,
                               str_x, str_r, str_rsw,
                               (void *)chunk_va_start,
                               (void *)(chunk_va_start +
                                        (chunk_count - 1) * PGSIZE),
                               (void *)chunk_pa_start,
                               (void *)(chunk_pa_start +
                                        (chunk_count - 1) * PGSIZE),
                               chunk_count);
                    }
                }
                if (valid_entry) {
                    chunk_start = i;
                    chunk_va_start = va;
                    chunk_pa_start = pa;
                    chunk_flags = flags;
                    chunk_count = 1;
                } else {
                    chunk_start = -1;
                }
            }
        }
    } else {
        for (int i = idx_start; i < idx_end; i++) {
            pte_t pte = pagetable[i];
            if (pte & PTE_V) {
                uint64 va = va_base | (((uint64)i) << (12 + 9 * level));
                if (omit_pa && va >= (uint64)PA2VA(KERNBASE) && va < (uint64)PA2VA(PHYSTOP))
                    continue;
                void *pa = (void *)PTE2PA(pte);
                const char *str_v = (pte & PTE_V) ? "V" : " ";
                const char *str_u = (pte & PTE_U) ? "U" : " ";
                const char *str_w = (pte & PTE_W) ? "W" : " ";
                const char *str_x = (pte & PTE_X) ? "X" : " ";
                const char *str_r = (pte & PTE_R) ? "R" : " ";
                const char *str_rsw = " ";
                printf("%*sPTE[%d](%p): %x(%s%s%s%s%s%s), (va, pa): (%p, %p)",
                       indent, "", i, &pagetable[i], (uint32)PTE_FLAGS(pte),
                       str_v, str_u, str_w, str_x, str_r, str_rsw,
                       (void *)va, pa);
                if (level > 0 && PTE_FLAGS(pte) == PTE_V) {
                    printf(":\n");
                    dump_pagetable((pagetable_t)PA2VA(pa), level - 1, indent + 2, va,
                                   0, omit_pa);
                } else {
                    printf("\n");
                }
            }
        }
    }
}
