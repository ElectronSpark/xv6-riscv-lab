/**
 * @file uart.h
 * @brief UART interface with runtime-configurable addresses from device tree.
 */

#ifndef __KERNEL_UART_H
#define __KERNEL_UART_H

#include "types.h"

extern uint64 __uart0_mmio_base;
extern uint64 __uart0_irqno;
extern uint32 __uart0_clock;      // Hz, 0 = default
extern uint32 __uart0_baud;       // 0 = 115200
extern uint32 __uart0_reg_shift;  // 0 = 1-byte, 2 = 4-byte spacing
extern uint32 __uart0_reg_io_width; // 1 = 8-bit, 4 = 32-bit access

#define UART0 __uart0_mmio_base
#define UART0_IRQ __uart0_irqno

#endif /* __KERNEL_UART_H */
