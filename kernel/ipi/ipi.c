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

// Per-CPU state, placed in special linker section for trampoline access
__SECTION(cpu_local_sec)
__ALIGNED_PAGE
struct cpu_local cpus[NCPU] = {0};

// IRQ number for supervisor software interrupt
#define IRQ_S_SOFT  1

// External: boot hart ID from start.c
extern _Atomic int boot_hartid;

// IPI statistics per hart
static volatile uint64 ipi_received_count[NCPU];
static volatile uint64 ipi_sent_count[NCPU];

/**
 * IPI handler - called when a hart receives a software interrupt.
 * This clears the interrupt pending bit and processes the IPI.
 */
static void ipi_handler(int irq, void *data, device_t *dev) {
    int hartid = cpuid();
    
    // Clear the software interrupt pending bit (SIP.SSIP)
    // Must be done to acknowledge the interrupt
    w_sip(r_sip() & ~SIE_SSIE);
    
    // Increment received counter
    __atomic_add_fetch(&ipi_received_count[hartid], 1, __ATOMIC_RELAXED);
}

/**
 * Initialize the IPI subsystem.
 */
void ipi_init(void) {
    // Initialize counters
    for (int i = 0; i < NCPU; i++) {
        ipi_received_count[i] = 0;
        ipi_sent_count[i] = 0;
    }
    
    // Register the IPI handler for supervisor software interrupt
    struct irq_desc ipi_desc = {
        .handler = ipi_handler,
        .data = NULL,
        .dev = NULL,
    };
    
    int ret = register_irq_handler(IRQ_S_SOFT, &ipi_desc);
    if (ret < 0) {
        printf("ipi_init: failed to register IPI handler: %d\n", ret);
        return;
    }
    
    printf("ipi_init: IPI subsystem initialized (IRQ %d)\n", IRQ_S_SOFT);
}

/**
 * Send an IPI to a specific hart.
 */
int ipi_send_single(int hartid, struct ipi_msg *msg) {
    if (hartid < 0 || hartid >= NCPU) {
        return -EINVAL;
    }
    
    unsigned long hart_mask = 1UL << hartid;
    long ret = sbi_send_ipi(hart_mask, 0);
    
    if (ret == 0) {
        __atomic_add_fetch(&ipi_sent_count[cpuid()], 1, __ATOMIC_RELAXED);
    }
    
    return (int)ret;
}

/**
 * Send an IPI to multiple harts specified by a mask.
 */
int ipi_send_mask(unsigned long hart_mask, unsigned long hart_mask_base) {
    long ret = sbi_send_ipi(hart_mask, hart_mask_base);
    
    if (ret == 0) {
        // Count number of bits set in mask for statistics
        unsigned long mask = hart_mask;
        while (mask) {
            if (mask & 1) {
                __atomic_add_fetch(&ipi_sent_count[cpuid()], 1, __ATOMIC_RELAXED);
            }
            mask >>= 1;
        }
    }
    
    return (int)ret;
}

/**
 * Send an IPI to all harts except the calling hart.
 */
int ipi_send_all_but_self(void) {
    int my_hart = cpuid();
    unsigned long hart_mask = 0;
    
    for (int i = 0; i < NCPU; i++) {
        if (i != my_hart) {
            hart_mask |= (1UL << i);
        }
    }
    
    return ipi_send_mask(hart_mask, 0);
}

/**
 * Send an IPI to all harts including the calling hart.
 */
int ipi_send_all(void) {
    unsigned long hart_mask = 0;
    
    for (int i = 0; i < NCPU; i++) {
        hart_mask |= (1UL << i);
    }
    
    return ipi_send_mask(hart_mask, 0);
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

