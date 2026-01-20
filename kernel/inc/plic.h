#ifndef __KERNEL_PLATFORM_LEVEL_INTERRUPT_CONTROLLER_H
#define __KERNEL_PLATFORM_LEVEL_INTERRUPT_CONTROLLER_H

#include "types.h"


// qemu puts platform-level interrupt controller (PLIC) here.
extern uint64 __plic_mmio_base;
#define PLIC __plic_mmio_base
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)


#endif// Platform Level Interrupt Controller (PLIC) definitions
