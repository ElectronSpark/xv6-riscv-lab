#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "percpu.h"
#include "trap.h"
#include "uart.h"
#include "virtio.h"
#include "plic.h"
#include "timer/goldfish_rtc.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//

uint64 __plic_mmio_base = 0x0c000000L;

void
plicinit(void)
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32*)(PLIC + UART0_IRQ*4) = 1;
  *(uint32*)(PLIC + VIRTIO0_IRQ*4) = 1;
  *(uint32*)(PLIC + GOLDFISH_RTC_IRQ*4) = 1;  // Goldfish RTC

  // PCIE IRQs are 32 to 35
  for(int irq = 1; irq < 0x35; irq++){
    // TODO
    *(uint32*)(PLIC + irq*4) = 1;
  }
}

void
plicinithart(void)
{
  int hart = cpuid();
  
  // set enable bits for this hart's S-mode
  // for the uart, virtio disk, and Goldfish RTC.
  *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ) | (1 << VIRTIO1_IRQ) | (1 << GOLDFISH_RTC_IRQ);

  // set this hart's S-mode priority threshold to 0.
  *(uint32*)PLIC_SPRIORITY(hart) = 0;

  // hack to get at next 32 IRQs for e1000
  *(uint32*)(PLIC_SENABLE(hart)+4) = 0xffffffff;
}

// ask the PLIC what interrupt we should serve.
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *(uint32*)PLIC_SCLAIM(hart);
  return irq;
}

// tell the PLIC we've served this IRQ.
void
plic_complete(int irq)
{
  int hart = cpuid();
  *(uint32*)PLIC_SCLAIM(hart) = irq;
}
