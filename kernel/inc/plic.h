#ifndef __KERNEL_PLATFORM_LEVEL_INTERRUPT_CONTROLLER_H
#define __KERNEL_PLATFORM_LEVEL_INTERRUPT_CONTROLLER_H

#include "types.h"
#include "bits.h"

// context 0: M Mode Hart 0
// context 1: S Mode Hart 0
// context 2: M Mode Hart 1
// context 3: S Mode Hart 1
// ...

// qemu puts platform-level interrupt controller (PLIC) here.
extern uint64 __plic_mmio_base;
#define PLIC __plic_mmio_base
#define PLIC_PRIORITY_BASE (PLIC + 0x0)
#define PLIC_PENDING_BASE (PLIC + 0x1000)
#define PLIC_ENABLE_BASE (PLIC + 0x2000)
#define PLIC_PRIORITY_THRESH_BASE (PLIC + 0x200000)
#define PLIC_CLAIM_BASE (PLIC + 0x200004)

#define PLIC_CONTEXT_ENABLE(context) (PLIC + 0x2000 + ((context) << 7))
#define PLIC_MENABLE(hart) (PLIC_ENABLE_BASE + ((hart) << 8))
#define PLIC_SENABLE(hart) (PLIC_MENABLE(hart) + 0x80)
#define PLIC_CONTEXT_PRIORITY_THRESH(hart) (PLIC_PRIORITY_THRESH_BASE + ((hart) << 12))
#define PLIC_MPRIORITY_THRESH(hart) (PLIC_PRIORITY_THRESH_BASE + ((hart) << 13))
#define PLIC_SPRIORITY_THRESH(hart) (PLIC_MPRIORITY_THRESH(hart) + 0x1000)
#define PLIC_CONTEXT_CLAIM(hart) (PLIC_CLAIM_BASE + ((hart) << 12))
#define PLIC_MCLAIM(hart) (PLIC_CLAIM_BASE + ((hart) << 13))
#define PLIC_SCLAIM(hart) (PLIC_MCLAIM(hart) + 0x1000)

// Get the address of the priority register for a given IRQ
#define PLIC_PRIORITY(__IRQ) ((uint32 *)(PLIC_PRIORITY_BASE + ((__IRQ) << 2)))
// Get the pending status of an IRQ
#define PLIC_PENDING(__IRQ) bits_test_bit32((const void *)PLIC_PENDING_BASE, (__IRQ))
// Set the pending bit of an IRQ
#define PLIC_SET_PENDING(__IRQ) bits_test_and_set_bit32((void *)PLIC_PENDING_BASE, (__IRQ))
// Clear the pending bit of an IRQ
#define PLIC_CLEAR_PENDING(__IRQ) bits_test_and_clear_bit32((void *)PLIC_PENDING_BASE, (__IRQ))
// Get the enable bit for a given IRQ and hart
#define PLIC_SENABLED(hart, __IRQ) bits_test_bit32((const void *)PLIC_SENABLE(hart), (__IRQ))
// Set the enable bit for a given IRQ and hart
#define PLIC_SET_SENABLE(hart, __IRQ) bits_test_and_set_bit32((void *)PLIC_SENABLE(hart), (__IRQ))
// Clear the enable bit for a given IRQ and hart
#define PLIC_CLEAR_SENABLE(hart, __IRQ) bits_test_and_clear_bit32((void *)PLIC_SENABLE(hart), (__IRQ))

#endif // Platform Level Interrupt Controller (PLIC) definitions
