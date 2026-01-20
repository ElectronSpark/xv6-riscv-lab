#ifndef __KERNEL_UART_H
#define __KERNEL_UART_H

#include "types.h"

// qemu puts UART registers here in physical memory.
extern uint64 __uart0_mmio_base;
extern uint64 __uart0_irqno;
#define UART0 __uart0_mmio_base
#define UART0_IRQ __uart0_irqno

#endif /* __KERNEL_UART_H */
