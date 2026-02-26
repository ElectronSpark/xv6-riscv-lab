/**
 * @file lapic.c
 * @brief x86_64 Local APIC driver.
 *
 * Detects the LAPIC via MSR_APIC_BASE, enables it, configures the
 * spurious-interrupt vector, and provides EOI / register access.
 *
 * The LAPIC timer is NOT configured here — that will be done later
 * when per-CPU timer support is needed (currently using PIT via IOAPIC).
 */

#include "types.h"
#include "printf.h"
#include "seg.h"    /* rdmsr, wrmsr, MSR_APIC_BASE */
#include "lapic.h"

/* APIC base MSR bits */
#define APIC_BASE_ENABLE    (1ULL << 11)
#define APIC_BASE_BSP       (1ULL << 8)
#define APIC_BASE_ADDR_MASK 0xFFFFFFFFF000ULL

/* LAPIC MMIO base virtual address (identity mapped) */
static volatile uint32 *lapic_base;

uint32 lapic_read(uint32 reg)
{
    return lapic_base[reg / 4];
}

void lapic_write(uint32 reg, uint32 val)
{
    lapic_base[reg / 4] = val;
    /* Read-back to ensure the write is serialized (Intel SDM recommendation) */
    (void)lapic_base[LAPIC_ID / 4];
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

int lapic_id(void)
{
    return (int)(lapic_read(LAPIC_ID) >> 24);
}

/* debugcon single-character output for early tracing */
static inline void lapic_dbg(char c)
{
    asm volatile("outb %0, %1" : : "a"((uint8)c), "Nd"((uint16)0xE9));
}

void lapic_init(void)
{
    /* Read the APIC base address from MSR */
    uint64 apic_msr = rdmsr(MSR_APIC_BASE);
    uint64 base_addr = apic_msr & APIC_BASE_ADDR_MASK;

    /* Ensure APIC is globally enabled in the MSR */
    if (!(apic_msr & APIC_BASE_ENABLE)) {
        apic_msr |= APIC_BASE_ENABLE;
        wrmsr(MSR_APIC_BASE, apic_msr);
    }

    lapic_base = (volatile uint32 *)(base_addr);

    /* Clear any pending errors by writing (then reading) ESR */
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);

    /* Set the Spurious Interrupt Vector Register:
     *   - Enable the APIC (bit 8)
     *   - Spurious vector = 0xFF */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);

    /* Mask all LVT entries initially */
    lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR,   LAPIC_ERROR_VEC);  /* error vector unmasked */

    /* If the LAPIC version supports the Thermal and PerfCnt LVT entries,
     * mask them too.  The version register bits [23:16] give the number
     * of LVT entries minus 1. */
    uint32 ver = lapic_read(LAPIC_VER);
    int max_lvt = ((ver >> 16) & 0xFF) + 1;
    if (max_lvt >= 5)
        lapic_write(LAPIC_LVT_PERFCNT, LAPIC_LVT_MASKED);
    if (max_lvt >= 6)
        lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);

    /* Set Task Priority to 0 — accept all interrupts */
    lapic_write(LAPIC_TPR, 0);

    /* Clear any stale EOIs */
    lapic_eoi();

    printf("[x86] LAPIC: id=%d ver=0x%x base=0x%lx max_lvt=%d\n",
           lapic_id(), ver & 0xFF, base_addr, max_lvt);
}
