#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "percpu.h"
#include "trap.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "dev/plic.h"
#include "timer/goldfish_rtc.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//

uint64 __plic_mmio_base = 0x0c000000L;

void
plicinit(void)
{
  // Core PLIC initialization
  // Device-specific IRQ priorities are set by each device's init function
  // using plic_enable_irq()
}

void
plicinithart(void)
{
  int hart = cpuid();
  
  // Set this hart's S-mode priority threshold to 0.
  // Device-specific IRQ enables are done by each device's init function
  // using plic_enable_irq()
  *PLIC_SPRIORITY_THRESH(hart) = 0;
}

// ask the PLIC what interrupt we should serve.
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *PLIC_SCLAIM(hart);
  return irq;
}

// tell the PLIC we've served this IRQ.
void
plic_complete(int irq)
{
  int hart = cpuid();
  *PLIC_SCLAIM(hart) = irq;
}

// Enable a specific IRQ on PLIC for all harts
void
plic_enable_irq(int irq)
{
  *PLIC_PRIORITY(irq) = 1;
  for (int hart = 0; hart < NCPU; hart++) {
    PLIC_SET_SENABLE(hart, irq);
  }
}
