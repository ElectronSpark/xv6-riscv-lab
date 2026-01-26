/**
 * IPI (Inter-Processor Interrupt) implementation for RISC-V
 * 
 * This module handles inter-processor interrupts using the SBI IPI extension.
 * IPIs are delivered as supervisor software interrupts (IRQ 1).
 */

#include "compiler.h"
#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc/sched.h"
#include "trap.h"
#include "sbi.h"
#include <smp/ipi.h>
#include "errno.h"
#include "proc/proc.h"
#include <smp/percpu.h>
#include "string.h"
#include "bits.h"

// Per-CPU state, placed in special linker section for trampoline access
__SECTION(cpu_local_sec)
__ALIGNED_PAGE
struct cpu_local cpus[NCPU] = {0};

uint64 ipi_pending[NCPU] = {0}; // Pending IPI bitmask per hart

// IRQ number for supervisor software interrupt
#define IRQ_S_SOFT  1

/**
 * IPI handler - called when a hart receives a software interrupt.
 * This clears the interrupt pending bit and processes the IPI.
 */
static void __ipi_irq_handler(int irq, void *data, device_t *dev) {
    // Clear the software interrupt pending bit (SIP.SSIP)
    // Must be done to acknowledge the interrupt
    w_sip(r_sip() & ~SIE_SSIE);

    int hartid = cpuid();
    uint64 pending = smp_load_acquire(&ipi_pending[hartid]);
    int reason = bits_ctz8((uint8)pending);

    if (reason < 0 || reason >= NR_IPI_REASON) {
        // No valid pending IPI
        return;
    }

    switch (reason) {
    case IPI_REASON_CRASH:
        // propogate crash to all other harts
        ipi_send_all_but_self(IPI_REASON_CRASH);
        for(;;) {
            asm volatile("wfi");
        }
        break;
    case IPI_REASON_CALL_FUNC:
        // Request to call a function - not implemented yet
        break;
    case IPI_REASON_RESCHEDULE:
        // Request to reschedule
        // sched_yield();
        break;
    case IPI_REASON_TLB_FLUSH:
        // Request to flush TLB
        // Since XV6 use different page tables for kernel and user,
        // TLB will be flushed when returning to user mode.
        break;
    case IPI_REASON_GENERIC:
        // Generic IPI - no specific action
        break;
    default:
        // Unknown reason
        break;
    }

    // Clear the processed IPI reason bit
    atomic_and(&ipi_pending[hartid], ~(1UL << reason));
}

/**
 * Initialize the IPI subsystem.
 */
void ipi_init(void) {    
    // Register the IPI handler for supervisor software interrupt
    struct irq_desc ipi_desc = {
        .handler = __ipi_irq_handler,
        .data = NULL,
        .dev = NULL,
    };

    for (int i = 0; i < NCPU; i++) {
        smp_store_release(&ipi_pending[i], 0);
    }
    
    int ret = register_irq_handler(IRQ_S_SOFT, &ipi_desc);
    assert(ret == 0, "ipi_init: failed to register IPI handler: %d\n", ret);
    printf("ipi_init: IPI subsystem initialized (IRQ %d)\n", IRQ_S_SOFT);
}

/**
 * Send an IPI to a specific hart.
 */
int ipi_send_single(int hartid, int reason) {
    if (hartid < 0 || hartid >= NCPU) {
        return -EINVAL;
    }
    if (reason < 0 || reason >= NR_IPI_REASON) {
        return -EINVAL;
    }

    atomic_or(&ipi_pending[hartid], 1UL << reason);
    unsigned long hart_mask = 1UL << hartid;
    long ret = sbi_send_ipi(hart_mask, 0);
    
    return (int)ret;
}

/**
 * Send an IPI to multiple harts specified by a mask.
 * Bits out of bounds are ignored.
 */
int ipi_send_mask(unsigned long hart_mask, unsigned long hart_mask_base, int reason) {
    if (reason < 0 || reason >= NR_IPI_REASON) {
        return -EINVAL;
    }

    // Set the pending IPI bits for target harts
    for (int i = 0; i < NCPU; i++) {
        if (hart_mask & (1UL << i)) {
            atomic_or(&ipi_pending[i + hart_mask_base], 1UL << reason);
        }
    }
    
    return (int)sbi_send_ipi(hart_mask, hart_mask_base);
}

/**
 * Send an IPI to all harts except the calling hart.
 */
int ipi_send_all_but_self(int reason) {
    cpumask_t hart_mask = ((1UL << NCPU) - 1) & ~(1UL << cpuid());
    return ipi_send_mask(hart_mask, 0, reason);
}

/**
 * Send an IPI to all harts including the calling hart.
 */
int ipi_send_all(int reason) {
    cpumask_t hart_mask = (1UL << NCPU) - 1;
    
    for (int i = 0; i < NCPU; i++) {
        atomic_or(&ipi_pending[i], 1UL << reason);
    }
    
    return ipi_send_mask(hart_mask, 0, reason);
}

void cpus_init(void) {
    memset(cpus, 0, sizeof(cpus));
}

void mycpu_init(uint64 hartid, bool trampoline) {
  if (trampoline) {
    // Convert physical address to virtual address in trampoline region
    // Keep the offset within the page, but change to TRAMPOLINE_CPULOCAL base
    uint64 offset = (uint64)&cpus[hartid] & PAGE_MASK;
    uint64 c = TRAMPOLINE_CPULOCAL + offset;
    w_tp(c);
    printf("hart %ld mycpu_init: setting tp to %p - %p\n", 
            hartid, 
            (void *)c, 
            (void *)(c + sizeof(struct cpu_local)));
  } else {
    struct cpu_local *c = &cpus[hartid];
    w_tp((uint64)c);
    printf("hart %ld mycpu_init: setting tp to %p - %p\n", 
            hartid, 
            (void *)c, 
            (void *)((uint64)c + sizeof(struct cpu_local)));
  }
}

