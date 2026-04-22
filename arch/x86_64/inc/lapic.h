/**
 * @file lapic.h
 * @brief x86_64 Local APIC definitions and API.
 *
 * Register offsets, vector assignments, and public functions for the
 * Local Advanced Programmable Interrupt Controller.
 */

#ifndef _X86_64_LAPIC_H
#define _X86_64_LAPIC_H

#include "types.h"

/* ── LAPIC register offsets (from MMIO base) ── */
#define LAPIC_ID            0x020   /* Local APIC ID */
#define LAPIC_VER           0x030   /* Version */
#define LAPIC_TPR           0x080   /* Task Priority Register */
#define LAPIC_EOI           0x0B0   /* End of Interrupt */
#define LAPIC_LDR           0x0D0   /* Logical Destination Register */
#define LAPIC_DFR           0x0E0   /* Destination Format Register */
#define LAPIC_SVR           0x0F0   /* Spurious Interrupt Vector Register */
#define LAPIC_ISR_BASE      0x100   /* In-Service Register (8 × 32-bit) */
#define LAPIC_TMR_BASE      0x180   /* Trigger Mode Register */
#define LAPIC_IRR_BASE      0x200   /* Interrupt Request Register */
#define LAPIC_ESR           0x280   /* Error Status Register */
#define LAPIC_ICR_LO        0x300   /* Interrupt Command Register [31:0] */
#define LAPIC_ICR_HI        0x310   /* Interrupt Command Register [63:32] */
#define LAPIC_LVT_TIMER     0x320   /* LVT Timer Register */
#define LAPIC_LVT_THERMAL   0x330   /* LVT Thermal Sensor */
#define LAPIC_LVT_PERFCNT   0x340   /* LVT Performance Counter */
#define LAPIC_LVT_LINT0     0x350   /* LVT LINT0 */
#define LAPIC_LVT_LINT1     0x360   /* LVT LINT1 */
#define LAPIC_LVT_ERROR     0x370   /* LVT Error */
#define LAPIC_TIMER_ICR     0x380   /* Timer Initial Count */
#define LAPIC_TIMER_CCR     0x390   /* Timer Current Count */
#define LAPIC_TIMER_DCR     0x3E0   /* Timer Divide Configuration */

/* ── SVR bits ── */
#define LAPIC_SVR_ENABLE    (1 << 8)    /* APIC Software Enable */

/* ── LVT mask bit ── */
#define LAPIC_LVT_MASKED    (1 << 16)

/* ── Timer modes (bits 18:17 of LVT Timer) ── */
#define LAPIC_TIMER_ONESHOT     (0 << 17)
#define LAPIC_TIMER_PERIODIC    (1 << 17)
#define LAPIC_TIMER_TSCDEADLINE (2 << 17)

/* ── Timer divide values ── */
#define LAPIC_TDCR_1        0xB
#define LAPIC_TDCR_2        0x0
#define LAPIC_TDCR_4        0x1
#define LAPIC_TDCR_8        0x2
#define LAPIC_TDCR_16       0x3
#define LAPIC_TDCR_32       0x8
#define LAPIC_TDCR_64       0x9
#define LAPIC_TDCR_128      0xA

/* ── ICR delivery mode ── */
#define LAPIC_ICR_FIXED     (0 << 8)
#define LAPIC_ICR_INIT      (5 << 8)
#define LAPIC_ICR_STARTUP   (6 << 8)
#define LAPIC_ICR_LEVEL     (1 << 14)
#define LAPIC_ICR_ASSERT    (1 << 14)
#define LAPIC_ICR_DEASSERT  (0 << 14)
#define LAPIC_ICR_STATUS    (1 << 12)   /* Delivery status (read-only) */

/* ── Vector assignments ── */
#define T_IRQ0              32      /* I/O APIC IRQ 0 starts at vector 32 */
#define LAPIC_IPI_VEC       0xEE    /* IPI (inter-processor interrupt) (238) */
#define LAPIC_TIMER_VEC     0xEF    /* LAPIC timer (239) */
#define LAPIC_ERROR_VEC     0xF0    /* LAPIC error (240) */
#define LAPIC_SPURIOUS_VEC  0xFF    /* Spurious interrupt (255) */

/* ── ICR destination shorthand (bits [19:18]) ── */
#define LAPIC_ICR_DEST_NONE         (0 << 18)   /* use destination field */
#define LAPIC_ICR_DEST_SELF         (1 << 18)
#define LAPIC_ICR_DEST_ALL          (2 << 18)   /* all including self */
#define LAPIC_ICR_DEST_ALLBUTSELF   (3 << 18)   /* all excluding self */

/* ── Public API ── */

/** Initialize the Local APIC on the current CPU. */
void lapic_init(void);

/** Send End-Of-Interrupt to the Local APIC. */
void lapic_eoi(void);

/** Read the Local APIC ID of the current CPU. */
int lapic_id(void);

/** Read a LAPIC register. */
uint32 lapic_read(uint32 reg);

/** Write a LAPIC register. */
void lapic_write(uint32 reg, uint32 val);

#endif /* _X86_64_LAPIC_H */
