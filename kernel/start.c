#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "atomic.h"
#include "timer/timer.h"

void start_kernel(int hartid, void *fdt_base, bool is_boot_hart);
void timerinit();

// entry.S needs one stack per CPU.
// Must be aligned to KERNEL_STACK_SIZE so that idle_proc_init can find
// the stack base by masking the current SP.
__attribute__ ((aligned (KERNEL_STACK_SIZE))) char stack0[KERNEL_STACK_SIZE * NCPU];

// The hartid of the boot hart (set by the first hart to reach start())
_Atomic int boot_hartid = -1;

// entry.S jumps here in supervisor mode on stack0.
// When booting from OpenSBI:
//   - We're already in S-mode
//   - hartid is passed in a0 (already saved to tp in entry.S)
//   - dtb pointer is passed in a1
void
start(int hartid, void *fdt_base)
{
  bool is_boot_hart = false;
  // The first hart to get here becomes the boot hart
  is_boot_hart = atomic_cas(&boot_hartid, -1, hartid);

  // disable paging for now.
  w_satp(0);

  // enable supervisor-mode interrupts.
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // ask for clock interrupts.
  timerinit();

  // jump to main().
  start_kernel(hartid, fdt_base, is_boot_hart);
}

// ask each hart to generate timer interrupts.
// When using OpenSBI, the firmware has already configured:
//   - menvcfg STCE bit for sstc extension
//   - mcounteren for stimecmp and time access
// We just need to set up the first timer interrupt.
void
timerinit()
{
  // calculate jiff ticks.
  // One time calculation, thus no optimization needed.
  __jiff_ticks = TIMEBASE_FREQUENCY / HZ;

  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + JIFF_TICKS);
}
