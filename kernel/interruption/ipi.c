/**
 * IPI (Inter-Processor Interrupt) implementation for RISC-V
 * 
 * This module handles inter-processor interrupts using the SBI IPI extension.
 * IPIs are delivered as supervisor software interrupts (IRQ 1).
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "sched.h"
#include "trap.h"
#include "sbi.h"
#include "ipi.h"
#include "errno.h"
#include "proc.h"

// IRQ number for supervisor software interrupt
#define IRQ_S_SOFT  1

// External: boot hart ID from start.c
extern _Atomic int boot_hartid;

// IPI statistics per hart
static volatile uint64 ipi_received_count[NCPU];
static volatile uint64 ipi_sent_count[NCPU];

// Synchronization for IPI demo
static volatile int ipi_demo_phase = 0;  // 0: not started, 1: secondary->boot, 2: boot->secondary, 3: done
static volatile int secondary_ipi_count = 0;  // Count of IPIs received from secondary harts
static volatile int secondary_ready_count = 0; // Count of secondary harts ready to participate
static volatile int secondary_ack_count = 0;   // Count of secondary harts that received reply
static volatile int hart_sent_ipi[NCPU] = {0}; // Per-hart flag: already sent IPI in this demo round

/**
 * IPI handler - called when a hart receives a software interrupt.
 * This clears the interrupt pending bit and processes the IPI.
 */
static void ipi_handler(int irq, void *data, device_t *dev) {
    int hartid = cpuid();
    int boot_hart = __atomic_load_n(&boot_hartid, __ATOMIC_RELAXED);
    
    // Clear the software interrupt pending bit (SIP.SSIP)
    // Must be done to acknowledge the interrupt
    w_sip(r_sip() & ~SIE_SSIE);
    
    // Increment received counter
    __atomic_add_fetch(&ipi_received_count[hartid], 1, __ATOMIC_RELAXED);
    
    int phase = __atomic_load_n(&ipi_demo_phase, __ATOMIC_ACQUIRE);
    
    if (phase == 1 && hartid == boot_hart) {
        // Phase 1: Boot hart received IPI from a secondary hart
        __atomic_add_fetch(&secondary_ipi_count, 1, __ATOMIC_RELAXED);
    } else if (phase == 2 && hartid != boot_hart) {
        // Phase 2: Secondary hart received reply from boot hart
        __atomic_add_fetch(&secondary_ack_count, 1, __ATOMIC_RELAXED);
    }
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
int ipi_send_single(int hartid) {
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

/**
 * IPI demonstration function.
 * Shows inter-processor communication with ping-pong pattern:
 * 1. Secondary harts send IPI to boot hart
 * 2. Boot hart replies to all secondary harts
 */
void ipi_demo(void) {
    int my_hart = cpuid();
    int boot_hart = __atomic_load_n(&boot_hartid, __ATOMIC_RELAXED);
    
    printf("[IPI Demo] Starting IPI ping-pong demonstration...\n");
    printf("[IPI Demo] Boot hart is %d, current hart is %d\n", boot_hart, my_hart);
    
    // Reset demo state
    __atomic_store_n(&ipi_demo_phase, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&secondary_ipi_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&secondary_ready_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&secondary_ack_count, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < NCPU; i++) {
        hart_sent_ipi[i] = 0;
    }
    
    // ============ Phase 1: Secondary harts -> Boot hart ============
    printf("[IPI Demo] Phase 1: Waiting for secondary harts to send IPI to boot hart...\n");
    __atomic_store_n(&ipi_demo_phase, 1, __ATOMIC_RELEASE);
    
    // Build mask of secondary harts
    unsigned long secondary_mask = 0;
    for (int i = 0; i < NCPU; i++) {
        if (i != boot_hart) {
            secondary_mask |= (1UL << i);
        }
    }
    
    // Send wake-up IPI to secondary harts so they exit wfi and check the phase
    ipi_send_mask(secondary_mask, 0);
    
    // Wait for secondary harts to send their IPIs
    for (int i = 0; i < 30; i++) {  // Wait up to 3 seconds
        sleep_ms(100);
        int received = __atomic_load_n(&secondary_ipi_count, __ATOMIC_ACQUIRE);
        int ready = __atomic_load_n(&secondary_ready_count, __ATOMIC_ACQUIRE);
        if (received >= ready && ready > 0) break;  // All ready harts have sent
    }
    
    int received = __atomic_load_n(&secondary_ipi_count, __ATOMIC_ACQUIRE);
    printf("[IPI Demo] Boot hart received %d IPIs from secondary harts\n", received);
    
    // ============ Phase 2: Boot hart -> Secondary harts ============
    printf("[IPI Demo] Phase 2: Boot hart replying to secondary harts...\n");
    __atomic_store_n(&ipi_demo_phase, 2, __ATOMIC_RELEASE);
    
    int ret = ipi_send_mask(secondary_mask, 0);
    if (ret != 0) {
        printf("[IPI Demo] Failed to send reply IPI: %d\n", ret);
    }
    
    // Wait for replies to be processed
    sleep_ms(500);
    
    int acks = __atomic_load_n(&secondary_ack_count, __ATOMIC_ACQUIRE);
    printf("[IPI Demo] %d secondary harts acknowledged the reply\n", acks);
    
    // ============ Print statistics ============
    __atomic_store_n(&ipi_demo_phase, 3, __ATOMIC_RELEASE);  // Signal done
    
    printf("[IPI Demo] IPI statistics:\n");
    for (int i = 0; i < NCPU; i++) {
        uint64 received_cnt = __atomic_load_n(&ipi_received_count[i], __ATOMIC_RELAXED);
        uint64 sent = __atomic_load_n(&ipi_sent_count[i], __ATOMIC_RELAXED);
        if (received_cnt > 0 || sent > 0) {
            printf("  Hart %d: sent=%ld, received=%ld\n", i, sent, received_cnt);
        }
    }
    
    printf("[IPI Demo] IPI ping-pong demonstration complete.\n");
}

/**
 * Get the current IPI demo phase.
 */
int ipi_get_demo_phase(void) {
    return __atomic_load_n(&ipi_demo_phase, __ATOMIC_ACQUIRE);
}

/**
 * Called by secondary harts to send IPI to boot hart during demo phase 1.
 */
void ipi_secondary_send_to_boot(void) {
    int phase = __atomic_load_n(&ipi_demo_phase, __ATOMIC_ACQUIRE);
    if (phase != 1) {
        return;  // Not in phase 1, nothing to do
    }
    
    int hartid = cpuid();
    int boot_hart = __atomic_load_n(&boot_hartid, __ATOMIC_RELAXED);
    
    if (hartid == boot_hart) {
        return;  // Boot hart shouldn't call this
    }
    
    // Check if we already sent an IPI this round (atomic test-and-set)
    int already_sent = __atomic_exchange_n(&hart_sent_ipi[hartid], 1, __ATOMIC_ACQ_REL);
    if (already_sent) {
        return;  // Already sent, don't send again
    }
    
    // Mark ourselves as ready
    __atomic_add_fetch(&secondary_ready_count, 1, __ATOMIC_RELAXED);
    
    // Send IPI to boot hart
    unsigned long boot_mask = 1UL << boot_hart;
    long ret = sbi_send_ipi(boot_mask, 0);
    if (ret == 0) {
        __atomic_add_fetch(&ipi_sent_count[hartid], 1, __ATOMIC_RELAXED);
    } else {
        printf("[IPI] Hart %d failed to send IPI: %ld\n", hartid, ret);
    }
}
