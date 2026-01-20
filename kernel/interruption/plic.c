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
  *PLIC_PRIORITY(UART0_IRQ) = 1;
  *PLIC_PRIORITY(VIRTIO0_IRQ) = 1;
  *PLIC_PRIORITY(GOLDFISH_RTC_IRQ) = 1;  // Goldfish RTC

  // PCIE IRQs are 32 to 35
  for(int irq = 1; irq < 0x35; irq++){
    // TODO
    *PLIC_PRIORITY(irq) = 1;
  }
}

void
plicinithart(void)
{
  int hart = cpuid();
  
  // set enable bits for this hart's S-mode
  // for the uart, virtio disk, and Goldfish RTC.
  PLIC_SET_SENABLE(hart, UART0_IRQ);
  PLIC_SET_SENABLE(hart, VIRTIO0_IRQ);
  PLIC_SET_SENABLE(hart, VIRTIO1_IRQ);
  PLIC_SET_SENABLE(hart, GOLDFISH_RTC_IRQ);
  // *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ) | (1 << VIRTIO1_IRQ) | (1 << GOLDFISH_RTC_IRQ);

  // set this hart's S-mode priority threshold to 0.
  *PLIC_SPRIORITY_THRESH(hart) = 0;

  // hack to get at next 32 IRQs for e1000
  *(PLIC_SENABLE(hart)+4) = 0xffffffff;
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
