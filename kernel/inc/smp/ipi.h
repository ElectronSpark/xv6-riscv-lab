/**
 * @file ipi.h
 * @brief IPI (Inter-Processor Interrupt) handling for RISC-V
 *
 * This module provides functionality for sending and receiving
 * inter-processor interrupts between CPU harts using the SBI IPI extension.
 *
 * IPIs are used for:
 * - Crash propagation (halting all CPUs on panic)
 * - Remote function calls
 * - Rescheduling requests
 * - TLB shootdown
 *
 * The IPI subsystem uses a per-CPU pending bitmask to track which IPI
 * reasons are pending for each hart. This allows multiple IPI reasons
 * to be queued simultaneously.
 */

#ifndef __KERNEL_IPI_H
#define __KERNEL_IPI_H

#include "types.h"

/**
 * @defgroup ipi_reasons IPI Reason Codes
 * @brief Reason codes for inter-processor interrupts
 *
 * Each IPI carries a reason code indicating what action the target hart
 * should take. Reasons are stored as a bitmask, allowing multiple pending
 * IPIs per hart.
 * @{
 */
#define IPI_REASON_CRASH 0      /**< Halt hart on kernel panic */
#define IPI_REASON_CALL_FUNC 1  /**< Execute a remote function call */
#define IPI_REASON_RESCHEDULE 2 /**< Request scheduler to run */
#define IPI_REASON_TLB_FLUSH 3  /**< Flush TLB entries */
#define IPI_REASON_GENERIC 4    /**< Generic IPI (no specific action) */
#define NR_IPI_REASON 5         /**< Number of IPI reason codes */
/** @} */

BUILD_BUG_ON(NR_IPI_REASON > 8); /**< Limit to 8 reasons for bitmasking */

/**
 * @brief IPI callback function type
 *
 * Function pointer type for IPI callback handlers.
 * Currently unused but reserved for future IPI_REASON_CALL_FUNC support.
 */
typedef void (*ipi_callback_t)(void);

/**
 * Initialize the IPI subsystem.
 * Registers the software interrupt handler for IPIs.
 */
void ipi_init(void);

/**
 * Send an IPI to a specific hart.
 * @param hartid The target hart ID
 * @param reason The IPI reason code (IPI_REASON_*)
 * @return 0 on success, negative error code on failure
 */
int ipi_send_single(int hartid, int reason);

/**
 * Send an IPI to multiple harts specified by a mask.
 * @param hart_mask Bitmask of target harts (bit i = hart i)
 * @param hart_mask_base Base hart ID for the mask
 * @param reason The IPI reason code (IPI_REASON_*)
 * @return 0 on success, negative error code on failure
 */
int ipi_send_mask(unsigned long hart_mask, unsigned long hart_mask_base,
                  int reason);

/**
 * Send an IPI to all harts except the calling hart.
 * @param reason The IPI reason code (IPI_REASON_*)
 * @return 0 on success, negative error code on failure
 */
int ipi_send_all_but_self(int reason);

/**
 * Send an IPI to all harts including the calling hart.
 * @param reason The IPI reason code (IPI_REASON_*)
 * @return 0 on success, negative error code on failure
 */
int ipi_send_all(int reason);

#endif /* __KERNEL_IPI_H */
