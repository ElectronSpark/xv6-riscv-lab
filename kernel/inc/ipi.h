/**
 * IPI (Inter-Processor Interrupt) handling for RISC-V
 * 
 * This module provides functionality for sending and receiving
 * inter-processor interrupts between CPU harts using the SBI IPI extension.
 */

#ifndef __KERNEL_IPI_H
#define __KERNEL_IPI_H

#include "types.h"

// IPI reason codes - can be extended for different IPI purposes
#define IPI_REASON_GENERIC      0   // Generic IPI (no specific action)
#define IPI_REASON_RESCHEDULE   1   // Request target hart to reschedule
#define IPI_REASON_CALL_FUNC    2   // Request target hart to call a function
#define IPI_REASON_TLB_FLUSH    3   // Request target hart to flush TLB

// IPI callback function type
typedef void (*ipi_callback_t)(void *arg);

/**
 * Initialize the IPI subsystem.
 * Registers the software interrupt handler for IPIs.
 */
void ipi_init(void);

/**
 * Send an IPI to a specific hart.
 * @param hartid The target hart ID
 * @return 0 on success, negative error code on failure
 */
int ipi_send_single(int hartid);

/**
 * Send an IPI to multiple harts specified by a mask.
 * @param hart_mask Bitmask of target harts (bit i = hart i)
 * @param hart_mask_base Base hart ID for the mask
 * @return 0 on success, negative error code on failure
 */
int ipi_send_mask(unsigned long hart_mask, unsigned long hart_mask_base);

/**
 * Send an IPI to all harts except the calling hart.
 * @return 0 on success, negative error code on failure
 */
int ipi_send_all_but_self(void);

/**
 * Send an IPI to all harts including the calling hart.
 * @return 0 on success, negative error code on failure
 */
int ipi_send_all(void);

/**
 * Run an IPI demonstration showing inter-processor communication.
 * This function sends IPIs to all harts and prints messages.
 */
void ipi_demo(void);

/**
 * Called by secondary harts to participate in IPI demo.
 * Secondary harts should call this to send IPI to boot hart.
 */
void ipi_secondary_send_to_boot(void);

/**
 * Get the current IPI demo phase.
 * 0 = not active, 1 = secondary->boot, 2 = boot->secondary
 */
int ipi_get_demo_phase(void);

#endif /* __KERNEL_IPI_H */
