/**
 * @file seg.c
 * @brief x86_64 GDT, TSS, and SYSCALL/SYSRET initialization.
 *
 * Sets up the Global Descriptor Table with a SYSCALL/SYSRET-compatible
 * segment layout and per-CPU 64-bit Task State Segments.
 */

#include "types.h"
#include "seg.h"
#include "x86.h"
#include "memlayout.h"
#include "smp/percpu.h"

/*
 * GDT + per-CPU TSS structs are co-located in a single page-aligned page
 * so the whole page can be mapped at a high-canonical VA (CPU_ENTRY_GDT)
 * accessible under both kernel and user CR3.
 *
 * GDT layout:
 *   [0]           null
 *   [1]           kernel code 64  (0x08)
 *   [2]           kernel data     (0x10)
 *   [3]           user code 32    (0x18)
 *   [4]           user data       (0x20)
 *   [5]           user code 64    (0x28)
 *   [6+2*i, 7+2*i]  TSS for CPU i  (selector = 0x30 + i*16)
 */
static struct {
    uint64          gdt[NSEGS];
    struct tss64    tss[NCPU];
} __attribute__((aligned(4096))) cpu_entry_gdt_page;

#define gdt       (cpu_entry_gdt_page.gdt)

/* ── GDTR pseudo-descriptor ── */
static struct x86_desc_ptr gdtr;

/* Helpers for per-CPU TSS */
static inline struct tss64 *cpu_tss(int cpu)
{
    return &cpu_entry_gdt_page.tss[cpu];
}

static inline int tss_gdt_slot(int cpu)
{
    return GDT_BASE_SEGS + cpu * 2;
}

/*
 * Helper to install a 16-byte TSS descriptor at gdt[slot] and gdt[slot+1].
 */
static void gdt_install_tss(int slot, struct tss64 *tss)
{
    uint64 base = (uint64)tss;
    uint32 limit = sizeof(struct tss64) - 1;

    struct tss_desc *td = (struct tss_desc *)&gdt[slot];
    td->limit_lo       = (uint16)(limit & 0xFFFF);
    td->base_lo        = (uint16)(base & 0xFFFF);
    td->base_mid       = (uint8)((base >> 16) & 0xFF);
    td->access         = SEG_P | SEG_DPL(DPL_KERNEL) | TSS_TYPE_AVAIL;
    td->limit_hi_flags = (uint8)((limit >> 16) & 0x0F);
    td->base_mid2      = (uint8)((base >> 24) & 0xFF);
    td->base_hi        = (uint32)(base >> 32);
    td->reserved       = 0;
}

void x86_gdt_init(void)
{
    /* Null descriptor */
    gdt[0] = 0;

    /* Kernel Code 64-bit: Present, DPL 0, Executable, Readable, Long mode */
    gdt[1] = seg_desc(
        SEG_P | SEG_DPL(DPL_KERNEL) | SEG_S | SEG_E | SEG_RW,
        SEG_L | SEG_G
    );

    /* Kernel Data: Present, DPL 0, Writable */
    gdt[2] = seg_desc(
        SEG_P | SEG_DPL(DPL_KERNEL) | SEG_S | SEG_RW,
        SEG_G | SEG_DB
    );

    /* User Code 32-bit (compat placeholder): Present, DPL 3, Executable, Readable, 32-bit */
    gdt[3] = seg_desc(
        SEG_P | SEG_DPL(DPL_USER) | SEG_S | SEG_E | SEG_RW,
        SEG_DB | SEG_G
    );

    /* User Data: Present, DPL 3, Writable */
    gdt[4] = seg_desc(
        SEG_P | SEG_DPL(DPL_USER) | SEG_S | SEG_RW,
        SEG_G | SEG_DB
    );

    /* User Code 64-bit: Present, DPL 3, Executable, Readable, Long mode */
    gdt[5] = seg_desc(
        SEG_P | SEG_DPL(DPL_USER) | SEG_S | SEG_E | SEG_RW,
        SEG_L | SEG_G
    );

    /* TSS descriptors for all CPUs (each occupies 2 GDT slots) */
    for (int i = 0; i < NCPU; i++) {
        __builtin_memset(cpu_tss(i), 0, sizeof(struct tss64));
        cpu_tss(i)->iomap_base = sizeof(struct tss64);  /* no I/O bitmap */
        gdt_install_tss(tss_gdt_slot(i), cpu_tss(i));
    }

    /* Load the GDT */
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64)gdt;

    asm volatile("lgdt %0" : : "m"(gdtr));

    /* Reload CS via a far return */
    asm volatile(
        "pushq %[kcs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : [kcs] "i"((uint64)SEG_KCODE) : "rax", "memory"
    );

    /* Reload data segment registers */
    asm volatile(
        "movw %w[kds], %%ds\n\t"
        "movw %w[kds], %%es\n\t"
        "movw %w[kds], %%ss\n\t"
        "movw %w[null], %%fs\n\t"
        "movw %w[null], %%gs\n\t"
        : : [kds] "r"((uint16)SEG_KDATA), [null] "r"((uint16)SEG_NULL)
        : "memory"
    );

    /*
     * Intel clears the GS base hidden register when a null selector is
     * loaded.  Restore MSR_GS_BASE from the saved __x86_tp value so
     * that mycpu() / r_tp() keeps working.
     */
    if (__x86_tp)
        wrmsr(MSR_GS_BASE, __x86_tp);

    /* Load the TSS */
    asm volatile("ltr %w0" : : "r"((uint16)SEG_TSS));
}

void x86_gdt_remap(uint64 gdt_va)
{
    /* Update ALL per-CPU TSS descriptors to point to high-canonical VA */
    for (int i = 0; i < NCPU; i++) {
        uint64 tss_offset = (uint64)cpu_tss(i) - (uint64)&gdt[0];
        uint64 tss_va = gdt_va + tss_offset;

        struct tss_desc *td = (struct tss_desc *)&gdt[tss_gdt_slot(i)];
        td->base_lo   = (uint16)(tss_va & 0xFFFF);
        td->base_mid  = (uint8)((tss_va >> 16) & 0xFF);
        td->base_mid2 = (uint8)((tss_va >> 24) & 0xFF);
        td->base_hi   = (uint32)(tss_va >> 32);
    }

    /* Update GDTR to use the high-canonical VA */
    gdtr.base = gdt_va;
    asm volatile("lgdt %0" : : "m"(gdtr));

    /* Reload CS via far return */
    asm volatile(
        "pushq %[kcs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : [kcs] "i"((uint64)SEG_KCODE) : "rax", "memory"
    );

    /* Reload data segments */
    asm volatile(
        "movw %w[kds], %%ds\n\t"
        "movw %w[kds], %%es\n\t"
        "movw %w[kds], %%ss\n\t"
        "movw %w[null], %%fs\n\t"
        "movw %w[null], %%gs\n\t"
        : : [kds] "r"((uint16)SEG_KDATA), [null] "r"((uint16)SEG_NULL)
        : "memory"
    );

    /* Intel clears GS base on null selector load — restore it. */
    if (__x86_tp)
        wrmsr(MSR_GS_BASE, __x86_tp);

    /* Reload TSS (clear busy bit first by reinstalling as available) */
    struct tss_desc *td = (struct tss_desc *)&gdt[tss_gdt_slot(0)];
    td->access = SEG_P | SEG_DPL(DPL_KERNEL) | TSS_TYPE_AVAIL;
    asm volatile("ltr %w0" : : "r"((uint16)SEG_TSS));
}

uint64 x86_gdt_page_pa(void)
{
    return (uint64)&cpu_entry_gdt_page;
}

void x86_tss_set_rsp0(uint64 rsp0)
{
    cpu_tss(cpuid())->rsp0 = rsp0;
}

void x86_tss_set_rsp0_cpu(int cpu, uint64 rsp0)
{
    cpu_tss(cpu)->rsp0 = rsp0;
}

void x86_tss_set_ist1(uint64 ist1)
{
    cpu_tss(cpuid())->ist1 = ist1;
}

void x86_tss_set_ist2(uint64 ist2)
{
    cpu_tss(cpuid())->ist2 = ist2;
}

void x86_tss_set_ist1_cpu(int cpu, uint64 ist1)
{
    cpu_tss(cpu)->ist1 = ist1;
}

void x86_tss_set_ist2_cpu(int cpu, uint64 ist2)
{
    cpu_tss(cpu)->ist2 = ist2;
}

void x86_gdt_init_ap(void)
{
    int cpu = cpuid();

    /*
     * Save MSR_GS_BASE to a LOCAL variable before reloading segments.
     * __x86_tp is a global and is racy when multiple APs run concurrently.
     * r_tp() reads the per-CPU MSR_GS_BASE which is always correct here.
     */
    uint64 saved_gs_base = r_tp();

    /* Load the shared GDT (already set up and remapped by BSP) */
    asm volatile("lgdt %0" : : "m"(gdtr));

    /* Reload CS via a far return */
    asm volatile(
        "pushq %[kcs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : [kcs] "i"((uint64)SEG_KCODE) : "rax", "memory"
    );

    /* Reload data segment registers */
    asm volatile(
        "movw %w[kds], %%ds\n\t"
        "movw %w[kds], %%es\n\t"
        "movw %w[kds], %%ss\n\t"
        "movw %w[null], %%fs\n\t"
        "movw %w[null], %%gs\n\t"
        : : [kds] "r"((uint16)SEG_KDATA), [null] "r"((uint16)SEG_NULL)
        : "memory"
    );

    /* Intel clears GS base on null selector load — restore from LOCAL copy. */
    if (saved_gs_base)
        wrmsr(MSR_GS_BASE, saved_gs_base);

    /* Clear TSS busy bit and load this CPU's TSS */
    struct tss_desc *td = (struct tss_desc *)&gdt[tss_gdt_slot(cpu)];
    td->access = SEG_P | SEG_DPL(DPL_KERNEL) | TSS_TYPE_AVAIL;
    asm volatile("ltr %w0" : : "r"((uint16)SEG_TSS_CPU(cpu)));

    /* Set up SYSCALL/SYSRET MSRs for this CPU */
    x86_syscall_init();
}

void x86_syscall_init(void)
{
    /* Enable SYSCALL/SYSRET and NX in EFER */
    uint64 efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE | EFER_NXE;
    wrmsr(MSR_EFER, efer);

    /*
     * STAR MSR layout:
     *   [31:0]  = reserved (EIP for SYSCALL in compat mode, unused in 64-bit)
     *   [47:32] = kernel CS selector (SYSCALL: CS = this, SS = this + 8)
     *   [63:48] = user CS base (SYSRET: CS = this + 16, SS = this + 8)
     */
    uint64 star = ((uint64)SEG_KCODE << 32) | ((uint64)SEG_UCODE32 << 48);
    wrmsr(MSR_STAR, star);

    /*
     * LSTAR = kernel entry point for SYSCALL.
     */
    extern void vector0(void);
    extern void syscall_entry(void);
    uint64 trapvec_page0 = PGROUNDDOWN((uint64)vector0);
    uint64 syscall_lstar =
        TRAPVEC_ALIAS_BASE + ((uint64)syscall_entry - trapvec_page0);
    wrmsr(MSR_LSTAR, syscall_lstar);

    /*
     * SFMASK = RFLAGS bits to clear on SYSCALL.
     * Clear IF (disable interrupts), DF, TF, AC.
     */
    wrmsr(MSR_SFMASK, (1 << 9) | (1 << 10) | (1 << 8) | (1 << 18));
}
