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
#define IPI_REASON_CRASH        0   // Request target hart to crash (for testing)
#define IPI_REASON_CALL_FUNC    1   // Request target hart to call a function
#define IPI_REASON_RESCHEDULE   2   // Request target hart to reschedule
#define IPI_REASON_TLB_FLUSH    3   // Request target hart to flush TLB
#define IPI_REASON_GENERIC      4   // Generic IPI (no specific action)
#define NR_IPI_REASON           5   // Maximum reason code

BUILD_BUG_ON(NR_IPI_REASON > 8); // Limit to 8 reasons for bitmasking

// IPI callback function type
typedef void (*ipi_callback_t)(void);

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
int ipi_send_single(int hartid, int reason);

/**
 * Send an IPI to multiple harts specified by a mask.
 * @param hart_mask Bitmask of target harts (bit i = hart i)
 * @param hart_mask_base Base hart ID for the mask
 * @return 0 on success, negative error code on failure
 */
int ipi_send_mask(unsigned long hart_mask, unsigned long hart_mask_base, int reason);

/**
 * Send an IPI to all harts except the calling hart.
 * @return 0 on success, negative error code on failure
 */
int ipi_send_all_but_self(int reason);

/**
 * Send an IPI to all harts including the calling hart.
 * @return 0 on success, negative error code on failure
 */
int ipi_send_all(int reason);

#endif /* __KERNEL_IPI_H */
