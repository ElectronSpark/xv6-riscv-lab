/**
 * @file seg.c
 * @brief x86_64 GDT, TSS, and SYSCALL/SYSRET initialization.
 *
 * Sets up the Global Descriptor Table with a SYSCALL/SYSRET-compatible
 * segment layout and a 64-bit Task State Segment.
 */

#include "types.h"
#include "seg.h"

/* ── Per-CPU TSS ── */
static struct tss64 cpu_tss __attribute__((aligned(16)));

/* ── GDT (8 entries: null, kcode, kdata, ucode32, udata, ucode64, tss_lo, tss_hi) ── */
static uint64 gdt[NSEGS] __attribute__((aligned(16)));

/* ── GDTR pseudo-descriptor ── */
static struct x86_desc_ptr gdtr;

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

    /* TSS descriptor (occupies slots 6 and 7) */
    __builtin_memset(&cpu_tss, 0, sizeof(cpu_tss));
    cpu_tss.iomap_base = sizeof(struct tss64);  /* no I/O bitmap */
    gdt_install_tss(6, &cpu_tss);

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

    /* Load the TSS */
    asm volatile("ltr %w0" : : "r"((uint16)SEG_TSS));
}

void x86_tss_set_rsp0(uint64 rsp0)
{
    cpu_tss.rsp0 = rsp0;
}

void x86_syscall_init(void)
{
    /* Enable SYSCALL/SYSRET in EFER */
    uint64 efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
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
     * Will be set to the actual syscall handler later; for now point to a
     * placeholder that just does SYSRETQ (we haven't built the handler yet).
     */
    /* wrmsr(MSR_LSTAR, (uint64)syscall_entry); */

    /*
     * SFMASK = RFLAGS bits to clear on SYSCALL.
     * Clear IF (disable interrupts), DF, TF, AC.
     */
    wrmsr(MSR_SFMASK, (1 << 9) | (1 << 10) | (1 << 8) | (1 << 18));
}
