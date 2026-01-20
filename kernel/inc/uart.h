/**
 * @file uart.h
 * @brief UART (Universal Asynchronous Receiver-Transmitter) interface
 *
 * Provides runtime-configurable UART base addresses and IRQ numbers.
 * Device addresses are discovered at boot time from device tree or
 * platform-specific configuration, enabling multi-platform support.
 *
 * Platforms:
 *   - QEMU virt: UART at 0x10000000, IRQ 10
 *   - Orange Pi RV2: UART at 0x02500000, IRQ 18
 */

#ifndef __KERNEL_UART_H
#define __KERNEL_UART_H

#include "types.h"

/**
 * @brief UART MMIO base address (set at runtime)
 * @note Initialized during platform-specific boot sequence
 */
extern uint64 __uart0_mmio_base;

/**
 * @brief UART interrupt number (set at runtime)
 */
extern uint64 __uart0_irqno;

/** Convenience macro for UART base address */
#define UART0 __uart0_mmio_base
/** Convenience macro for UART IRQ number */
#define UART0_IRQ __uart0_irqno

#endif /* __KERNEL_UART_H */
