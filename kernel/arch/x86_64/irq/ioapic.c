/**
 * @file ioapic.c
 * @brief x86_64 I/O APIC driver.
 *
 * Manages the I/O APIC redirection table.  All entries start masked;
 * devices enable their IRQs via ioapic_enable() (called from
 * plic_enable_irq() on x86).
 *
 * The default MMIO base 0xFEC00000 matches QEMU's i440FX and Q35
 * chipsets.  This must be identity-mapped in the kernel page tables
 * (done by arch_vm_init in mm/vm.c).
 */

#include "types.h"
#include "printf.h"
#include "ioapic.h"
#include "lapic.h"

/* ── MMIO access ── */

static volatile uint32 *ioapic_base;

static uint32 ioapic_read(uint32 reg)
{
    ioapic_base[0] = reg;          /* IOREGSEL (offset 0x00) */
    return ioapic_base[4];         /* IOWIN   (offset 0x10) */
}

static void ioapic_write(uint32 reg, uint32 val)
{
    ioapic_base[0] = reg;          /* IOREGSEL */
    ioapic_base[4] = val;          /* IOWIN */
}

/* Number of redirection entries (set during init) */
static int ioapic_max_redir;

void ioapic_init(void)
{
    ioapic_base = (volatile uint32 *)IOAPIC_DEFAULT_BASE;

    uint32 ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_redir = ((ver >> 16) & 0xFF) + 1;

    uint32 id = (ioapic_read(IOAPIC_REG_ID) >> 24) & 0x0F;

    printf("[x86] IOAPIC: id=%d ver=0x%x max_redir=%d\n",
           id, ver & 0xFF, ioapic_max_redir);

    /* Mask all redirection entries by default */
    for (int i = 0; i < ioapic_max_redir; i++) {
        /* Write the low 32 bits: masked, edge-triggered, fixed delivery,
         * physical destination, vector = T_IRQ0 + i */
        ioapic_write(IOAPIC_REG_REDTBL(i),     IOAPIC_INT_MASKED | (T_IRQ0 + i));
        /* High 32 bits: destination LAPIC ID = 0 (BSP) */
        ioapic_write(IOAPIC_REG_REDTBL(i) + 1, 0);
    }
}

void ioapic_enable(int irq, int vector, int dest)
{
    if (irq < 0 || irq >= ioapic_max_redir)
        return;

    /* Low 32 bits: vector, fixed delivery, edge-triggered, active high,
     * physical destination, NOT masked (bit 16 clear) */
    uint32 lo = (uint32)vector;     /* bits 7:0 = vector */
    /* High 32 bits: destination LAPIC ID in bits 27:24 */
    uint32 hi = ((uint32)dest) << 24;

    ioapic_write(IOAPIC_REG_REDTBL(irq) + 1, hi);
    ioapic_write(IOAPIC_REG_REDTBL(irq),     lo);
}

/**
 * Enable an IRQ with level-triggered, active-low semantics.
 * Required for PCI device interrupts routed through the PIIX3.
 */
void ioapic_enable_level(int irq, int vector, int dest)
{
    if (irq < 0 || irq >= ioapic_max_redir)
        return;

    /* PCI interrupts: level-triggered, active-low */
    uint32 lo = (uint32)vector | IOAPIC_TRIGGER_LEVEL | IOAPIC_ACTIVE_LOW;
    uint32 hi = ((uint32)dest) << 24;

    ioapic_write(IOAPIC_REG_REDTBL(irq) + 1, hi);
    ioapic_write(IOAPIC_REG_REDTBL(irq),     lo);
}

void ioapic_disable(int irq)
{
    if (irq < 0 || irq >= ioapic_max_redir)
        return;
    /* Read-modify-write: set the mask bit */
    uint32 lo = ioapic_read(IOAPIC_REG_REDTBL(irq));
    lo |= IOAPIC_INT_MASKED;
    ioapic_write(IOAPIC_REG_REDTBL(irq), lo);
}
