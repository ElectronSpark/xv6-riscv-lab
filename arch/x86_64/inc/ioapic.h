/**
 * @file ioapic.h
 * @brief x86_64 I/O APIC definitions and API.
 *
 * Provides register access, redirection table programming, and
 * IRQ routing for the I/O Advanced Programmable Interrupt Controller.
 */

#ifndef _X86_64_IOAPIC_H
#define _X86_64_IOAPIC_H

#include "types.h"

/* ── Default I/O APIC MMIO base (QEMU i440FX/Q35) ── */
#define IOAPIC_DEFAULT_BASE     0xFEC00000ULL

/* ── I/O APIC register indices (via IOREGSEL/IOWIN) ── */
#define IOAPIC_REG_ID           0x00    /* I/O APIC ID */
#define IOAPIC_REG_VER          0x01    /* Version (+ max redir entry) */
#define IOAPIC_REG_ARB          0x02    /* Arbitration ID */
#define IOAPIC_REG_REDTBL(n)    (0x10 + 2 * (n))  /* Redir table lo */

/* ── Redirection table entry bits ── */
#define IOAPIC_INT_MASKED       (1ULL << 16)
#define IOAPIC_TRIGGER_LEVEL    (1ULL << 15)    /* 1 = level, 0 = edge */
#define IOAPIC_ACTIVE_LOW       (1ULL << 13)    /* 1 = low, 0 = high */
#define IOAPIC_LOGICAL_DEST     (1ULL << 11)    /* 1 = logical, 0 = physical */

/* Delivery modes (bits 10:8) */
#define IOAPIC_DELMOD_FIXED     (0ULL << 8)
#define IOAPIC_DELMOD_LOWEST    (1ULL << 8)
#define IOAPIC_DELMOD_NMI       (4ULL << 8)
#define IOAPIC_DELMOD_INIT      (5ULL << 8)
#define IOAPIC_DELMOD_EXTINT    (7ULL << 8)

/* Destination field is bits 63:56 of the 64-bit redir entry */
#define IOAPIC_DEST_SHIFT       56

/* ── Public API ── */

/** Initialize the I/O APIC.  Programs all redirection entries as masked. */
void ioapic_init(void);

/**
 * Enable (unmask) an IRQ on the I/O APIC.
 *
 * @param irq       ISA IRQ number (0-23).
 * @param vector    IDT vector to deliver (typically T_IRQ0 + irq).
 * @param dest      Destination LAPIC ID.
 */
void ioapic_enable(int irq, int vector, int dest);

/**
 * Disable (mask) an IRQ on the I/O APIC.
 *
 * @param irq       ISA IRQ number (0-23).
 */
void ioapic_disable(int irq);

/**
 * Enable an IRQ with level-triggered, active-low polarity.
 * Required for PCI device interrupts routed through the PIIX3.
 */
void ioapic_enable_level(int irq, int vector, int dest);

#endif /* _X86_64_IOAPIC_H */
