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

struct ipi_msg {
    ipi_callback_t callback;   // Function to call on IPI
    int reason;                // Reason code for the IPI
    void *arg;                 // Argument to pass to the callback
} __ALIGNED_CACHELINE;

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
int ipi_send_single(int hartid, struct ipi_msg *msg);

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

#endif /* __KERNEL_IPI_H */
