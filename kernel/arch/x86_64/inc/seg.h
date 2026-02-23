/**
 * @file seg.h
 * @brief x86_64 segmentation definitions: GDT selectors, TSS, descriptor types.
 *
 * GDT layout is designed for SYSCALL/SYSRET compatibility:
 *
 *   Index  Selector  Description
 *   ─────  ────────  ────────────────────────────────
 *     0    0x00      Null descriptor
 *     1    0x08      Kernel Code 64-bit  (DPL 0)
 *     2    0x10      Kernel Data          (DPL 0)
 *     3    0x18      User Code 32-bit     (DPL 3, compat placeholder)
 *     4    0x20      User Data            (DPL 3)
 *     5    0x28      User Code 64-bit     (DPL 3)
 *     6    0x30      TSS low              (system descriptor, 16 bytes)
 *     7    0x38      TSS high
 *
 * SYSCALL/SYSRET MSR STAR setup:
 *   STAR[47:32] = SEG_KCODE  (0x08)  → SYSCALL loads CS=0x08, SS=0x10
 *   STAR[63:48] = SEG_UCODE32 (0x18) → SYSRET loads CS=0x28|3, SS=0x20|3
 */

#ifndef _X86_64_SEG_H
#define _X86_64_SEG_H

#include "types.h"

/* ── GDT selectors ── */
#define SEG_NULL        0x00
#define SEG_KCODE       0x08    /* kernel code (ring 0, 64-bit) */
#define SEG_KDATA       0x10    /* kernel data (ring 0)         */
#define SEG_UCODE32     0x18    /* user code compat (ring 3)    */
#define SEG_UDATA       0x20    /* user data (ring 3)           */
#define SEG_UCODE       0x28    /* user code 64-bit (ring 3)    */
#define SEG_TSS         0x30    /* TSS descriptor (16 bytes)    */

#define NSEGS           8       /* total GDT entries (TSS uses 2) */

/* RPL (Requested Privilege Level) helpers */
#define SEG_RPL_MASK    0x03
#define SEG_RPL(sel)    ((sel) & SEG_RPL_MASK)

#define DPL_KERNEL      0
#define DPL_USER        3

/* ── Segment descriptor access byte bits ── */
#define SEG_A           (1 << 0)    /* Accessed */
#define SEG_RW          (1 << 1)    /* Readable (code) / Writable (data) */
#define SEG_DC          (1 << 2)    /* Direction/Conforming */
#define SEG_E           (1 << 3)    /* Executable */
#define SEG_S           (1 << 4)    /* Descriptor type: 1=code/data, 0=system */
#define SEG_DPL(dpl)    (((dpl) & 3) << 5)
#define SEG_P           (1 << 7)    /* Present */

/* Flags nibble (bits 52-55 of descriptor) */
#define SEG_L           (1 << 1)    /* Long mode (64-bit code) */
#define SEG_DB          (1 << 2)    /* Default operation size (32-bit) */
#define SEG_G           (1 << 3)    /* Granularity (4K pages) */

/* System segment types (for TSS) */
#define TSS_TYPE_AVAIL  0x9         /* 64-bit TSS (Available) */
#define TSS_TYPE_BUSY   0xB         /* 64-bit TSS (Busy)      */

/**
 * Build a GDT segment descriptor (8 bytes).
 * For 64-bit code, base and limit are ignored by the CPU but we set
 * limit=0xFFFFF, base=0 by convention.
 */
static inline uint64 seg_desc(uint8 access, uint8 flags)
{
    uint64 d = 0;
    /* limit 15:0 = 0xFFFF */
    d |= 0xFFFFULL;
    /* base 15:0 = 0 (already zero) */
    /* base 23:16 = 0, access byte */
    d |= ((uint64)access) << 40;
    /* limit 19:16 = 0xF, flags nibble */
    d |= ((uint64)(0xF | (flags << 4))) << 48;
    /* base 31:24 = 0 (already zero) */
    return d;
}

/* ── Task State Segment (64-bit) ── */
struct tss64 {
    uint32 reserved0;
    uint64 rsp0;        /* stack pointer for ring 0 */
    uint64 rsp1;        /* stack pointer for ring 1 */
    uint64 rsp2;        /* stack pointer for ring 2 */
    uint64 reserved1;
    uint64 ist1;        /* interrupt stack table entry 1 */
    uint64 ist2;
    uint64 ist3;
    uint64 ist4;
    uint64 ist5;
    uint64 ist6;
    uint64 ist7;
    uint64 reserved2;
    uint16 reserved3;
    uint16 iomap_base;  /* I/O map base address */
} __attribute__((packed));

/* ── GDTR / IDTR pseudo-descriptor ── */
struct x86_desc_ptr {
    uint16 limit;
    uint64 base;
} __attribute__((packed));

/* ── TSS descriptor (16 bytes, two GDT slots) ── */
struct tss_desc {
    uint16 limit_lo;
    uint16 base_lo;
    uint8  base_mid;
    uint8  access;     /* P, DPL, type */
    uint8  limit_hi_flags;
    uint8  base_mid2;
    uint32 base_hi;
    uint32 reserved;
} __attribute__((packed));

/* ── MSR addresses for SYSCALL/SYSRET ── */
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084

#define EFER_SCE        (1 << 0)    /* SYSCALL Enable */

/* ── GS base MSRs (for SWAPGS per-CPU data) ── */
#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

/* ── APIC MSR ── */
#define MSR_APIC_BASE   0x0000001B

/* MSR helpers */
static inline void wrmsr(uint32 msr, uint64 value)
{
    uint32 lo = (uint32)value;
    uint32 hi = (uint32)(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64 rdmsr(uint32 msr)
{
    uint32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64)hi << 32) | lo;
}

/* ── Public API ── */

/** Initialize the GDT with all segments and TSS, then load it. */
void x86_gdt_init(void);

/** Set the RSP0 (kernel stack) field in the TSS. */
void x86_tss_set_rsp0(uint64 rsp0);

/** Set the IST1 stack pointer in the TSS. */
void x86_tss_set_ist1(uint64 ist1);

/** Set up SYSCALL/SYSRET MSRs (STAR, LSTAR, SFMASK). */
void x86_syscall_init(void);

/* ──────────────────────────────────────────────────────────────
 *  IDT gate descriptor (16 bytes per entry)
 * ────────────────────────────────────────────────────────────── */

#define IDT_ENTRIES     256

/* Gate types (stored in type_attr field) */
#define GATE_INTERRUPT  0x8E    /* P=1, DPL=0, type=0xE (64-bit interrupt gate) */
#define GATE_TRAP       0x8F    /* P=1, DPL=0, type=0xF (64-bit trap gate)      */
#define GATE_USER_INT   0xEE    /* P=1, DPL=3, type=0xE (user-callable int gate) */

struct idt_gate {
    uint16 offset_lo;       /* target offset bits 15:0 */
    uint16 selector;        /* code segment selector */
    uint8  ist;             /* IST index (0 = legacy stack switching) */
    uint8  type_attr;       /* gate type + DPL + P */
    uint16 offset_mid;      /* target offset bits 31:16 */
    uint32 offset_hi;       /* target offset bits 63:32 */
    uint32 reserved;
} __attribute__((packed));

/**
 * Build one IDT gate descriptor.
 * @param gate      pointer to the IDT entry to fill in
 * @param handler   address of the assembly entry point
 * @param sel       code segment selector (usually SEG_KCODE)
 * @param type_attr gate type | DPL | P (e.g. GATE_INTERRUPT)
 * @param ist       IST index (0 for normal stack switching)
 */
static inline void idt_set_gate(struct idt_gate *gate, uint64 handler,
                                uint16 sel, uint8 type_attr, uint8 ist)
{
    gate->offset_lo  = (uint16)(handler & 0xFFFF);
    gate->selector   = sel;
    gate->ist        = ist & 0x7;
    gate->type_attr  = type_attr;
    gate->offset_mid = (uint16)((handler >> 16) & 0xFFFF);
    gate->offset_hi  = (uint32)(handler >> 32);
    gate->reserved   = 0;
}

/** Initialize the IDT with all 256 entries and load it via LIDT. */
void x86_idt_init(void);

/**
 * Remap GDT+TSS to a high-canonical virtual address.
 * Updates GDTR and TSS descriptor to use the new VA, then reloads.
 * @param gdt_va  high-canonical VA where the GDT page is mapped
 */
void x86_gdt_remap(uint64 gdt_va);

/**
 * Remap IDT to a high-canonical virtual address.
 * Updates IDTR to use the new VA and reloads.
 * @param idt_va  high-canonical VA where the IDT page is mapped
 */
void x86_idt_remap(uint64 idt_va);

/**
 * Return physical address of the page containing the GDT.
 * The TSS is co-located in the same page.
 */
uint64 x86_gdt_page_pa(void);

/**
 * Return physical address of the page containing the IDT.
 */
uint64 x86_idt_page_pa(void);

#endif /* _X86_64_SEG_H */
