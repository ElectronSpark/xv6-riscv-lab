/**
 * @file ipi.c
 * @brief IPI (Inter-Processor Interrupt) implementation for RISC-V
 *
 * This module handles inter-processor interrupts using the SBI IPI extension.
 * IPIs are delivered as supervisor software interrupts (IRQ 1).
 *
 * The implementation uses a per-CPU pending bitmask (ipi_pending[]) to track
 * which IPI reasons are pending for each hart. When an IPI is sent:
 * 1. The sender sets the appropriate bit in the target's ipi_pending
 * 2. The sender triggers a software interrupt via SBI
 * 3. The target's IPI handler reads ipi_pending to determine the reason
 * 4. The target clears the processed reason bit
 *
 * This allows multiple IPI reasons to be queued and processed in order.
 */

#include "compiler.h"
#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "trap.h"
#include "sbi.h"
#include <smp/ipi.h>
#include <smp/percpu.h>
#include "smp/percpu.h"
#include "errno.h"
#include "proc/thread.h"
#include "string.h"
#include "bits.h"
#include "lock/spinlock.h"

/** @brief Per-CPU state, placed in special linker section for trampoline access
 */
__SECTION(cpu_local_sec)
__ALIGNED_PAGE
struct cpu_local cpus[NCPU] = {0};

static cpumask_t cpu_active_mask = 0; /**< Bitmask of active CPUs */

/** @brief Pending IPI bitmask per hart (indexed by hart ID) */
uint64 ipi_pending[NCPU] = {0};

/** @brief IRQ number for supervisor software interrupt */
#define IRQ_S_SOFT 1

/**
 * IPI handler - called when a hart receives a software interrupt.
 * This clears the interrupt pending bit and processes ALL pending IPI reasons.
 */
static void __ipi_irq_handler(int irq, void *data, device_t *dev) {
    // Clear the software interrupt pending bit (SIP.SSIP)
    // Must be done to acknowledge the interrupt
    w_sip(r_sip() & ~SIE_SSIE);

    int hartid = cpuid();
    
    // Process all pending IPI reasons in a loop
    // Use atomic exchange to grab all pending reasons at once
    uint64 pending = __atomic_exchange_n(&ipi_pending[hartid], 0, __ATOMIC_ACQ_REL);
    
    while (pending != 0) {
        int reason = bits_ctz8((uint8)pending);
        if (reason < 0 || reason >= NR_IPI_REASON) {
            break;
        }
        
        // Clear this reason from our local copy
        pending &= ~(1UL << reason);

        switch (reason) {
        case IPI_REASON_CRASH:
            // Propagate crash to all other harts
            SET_CPU_CRASHED();
            panic_msg_lock();
            printf("[Core: %d] Received IPI_REASON_CRASH, crashing...\n", hartid);
            print_backtrace(r_fp(), KERNBASE, PHYSTOP);
            panic_msg_unlock();
            ipi_send_all_but_self(IPI_REASON_CRASH);
            for (;;) {
                asm volatile("wfi");
            }
            break;
        case IPI_REASON_CALL_FUNC:
            // Request to call a function - not implemented yet
            break;
        case IPI_REASON_RESCHEDULE:
            // Request to reschedule
            rq_flush_wake_list(cpuid());
            SET_NEEDS_RESCHED();
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
    }
}

/**
 * @brief Initialize the IPI subsystem.
 *
 * Registers the software interrupt handler for IPIs and clears
 * all pending IPI bitmasks.
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
 * @brief Send an IPI to a specific hart.
 * @param hartid The target hart ID (0 to NCPU-1)
 * @param reason The IPI reason code (IPI_REASON_*)
 * @return 0 on success, -EINVAL for invalid parameters
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
 * @brief Send an IPI to multiple harts specified by a mask.
 * @param hart_mask Bitmask of target harts (bit i = hart i)
 * @param hart_mask_base Base hart ID for the mask
 * @param reason The IPI reason code (IPI_REASON_*)
 * @return 0 on success, -EINVAL for invalid reason
 * @note Bits out of bounds are ignored.
 */
int ipi_send_mask(unsigned long hart_mask, unsigned long hart_mask_base,
                  int reason) {
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
 * @brief Send an IPI to all harts except the calling hart.
 * @param reason The IPI reason code (IPI_REASON_*)
 * @return 0 on success, negative error code on failure
 */
int ipi_send_all_but_self(int reason) {
    cpumask_t hart_mask = ((1UL << NCPU) - 1) & ~(1UL << cpuid());
    return ipi_send_mask(hart_mask, 0, reason);
}

/**
 * @brief Send an IPI to all harts including the calling hart.
 * @param reason The IPI reason code (IPI_REASON_*)
 * @return 0 on success, negative error code on failure
 */
int ipi_send_all(int reason) {
    cpumask_t hart_mask = (1UL << NCPU) - 1;

    for (int i = 0; i < NCPU; i++) {
        atomic_or(&ipi_pending[i], 1UL << reason);
    }

    return ipi_send_mask(hart_mask, 0, reason);
}

void cpus_init(void) { memset(cpus, 0, sizeof(cpus)); }

void mycpu_init(uint64 hartid, bool trampoline) {
    assert(hartid < NCPU, "mycpu_init: invalid hartid %ld", hartid);
    atomic_or(&cpu_active_mask, 1UL << hartid);
    if (trampoline) {
        // Convert physical address to virtual address in trampoline region
        // Keep the offset within the page, but change to TRAMPOLINE_CPULOCAL
        // base
        uint64 offset = (uint64)&cpus[hartid] & PAGE_MASK;
        uint64 c = TRAMPOLINE_CPULOCAL + offset;
        w_tp(c);
        printf("hart %ld mycpu_init: setting tp to %p - %p\n", hartid,
               (void *)c, (void *)(c + sizeof(struct cpu_local)));
    } else {
        struct cpu_local *c = &cpus[hartid];
        w_tp((uint64)c);
        printf("hart %ld mycpu_init: setting tp to %p - %p\n", hartid,
               (void *)c, (void *)((uint64)c + sizeof(struct cpu_local)));
    }
}

cpumask_t get_cpu_active_mask(void) {
    return smp_load_acquire(&cpu_active_mask);
}
