#include "compiler.h"
#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include <smp/atomic.h>
#include "timer/timer.h"
#include "arch/timer.h"

void start_kernel(int hartid, void *fdt_base, bool is_boot_hart);

// entry.S needs one stack per CPU.
// Must be aligned to KERNEL_STACK_SIZE so that idle_thread_init can find
// the stack base by masking the current SP.
__ALIGNED(KERNEL_STACK_SIZE)
__SECTION(".init.bss")
char stack0[KERNEL_STACK_SIZE * NCPU];

// Initial page table. Map 0~16GB and KERNEL_OFFSET~KERNEL_OFFSET+16GB to the
// same physical addresses with RW permissions.
__ALIGNED(PAGE_SIZE)
__SECTION(".init.data")
pte_t init_pgtbl[512] = {
    [0] = PA2PTE_INIT(0 << PXSHIFT(2)),
    [1] = PA2PTE_INIT(1 << PXSHIFT(2)),
    [2] = PA2PTE_INIT(2 << PXSHIFT(2)),
    [3] = PA2PTE_INIT(3 << PXSHIFT(2)),
    [4] = PA2PTE_INIT(4 << PXSHIFT(2)),
    [5] = PA2PTE_INIT(5 << PXSHIFT(2)),
    [6] = PA2PTE_INIT(6 << PXSHIFT(2)),
    [7] = PA2PTE_INIT(7 << PXSHIFT(2)),
    [8] = PA2PTE_INIT(8 << PXSHIFT(2)),
    [9] = PA2PTE_INIT(9 << PXSHIFT(2)),
    [10] = PA2PTE_INIT(10 << PXSHIFT(2)),
    [11] = PA2PTE_INIT(11 << PXSHIFT(2)),
    [12] = PA2PTE_INIT(12 << PXSHIFT(2)),
    [13] = PA2PTE_INIT(13 << PXSHIFT(2)),
    [14] = PA2PTE_INIT(14 << PXSHIFT(2)),
    [15] = PA2PTE_INIT(15 << PXSHIFT(2)),
    [256] = PA2PTE_INIT(0 << PXSHIFT(2)),
    [257] = PA2PTE_INIT(1 << PXSHIFT(2)),
    [258] = PA2PTE_INIT(2 << PXSHIFT(2)),
    [259] = PA2PTE_INIT(3 << PXSHIFT(2)),
    [260] = PA2PTE_INIT(4 << PXSHIFT(2)),
    [261] = PA2PTE_INIT(5 << PXSHIFT(2)),
    [262] = PA2PTE_INIT(6 << PXSHIFT(2)),
    [263] = PA2PTE_INIT(7 << PXSHIFT(2)),
    [264] = PA2PTE_INIT(8 << PXSHIFT(2)),
    [265] = PA2PTE_INIT(9 << PXSHIFT(2)),
    [266] = PA2PTE_INIT(10 << PXSHIFT(2)),
    [267] = PA2PTE_INIT(11 << PXSHIFT(2)),
    [268] = PA2PTE_INIT(12 << PXSHIFT(2)),
    [269] = PA2PTE_INIT(13 << PXSHIFT(2)),
    [270] = PA2PTE_INIT(14 << PXSHIFT(2)),
    [271] = PA2PTE_INIT(15 << PXSHIFT(2)),
};

// The hartid of the boot hart (set by the first hart to reach start())
__SECTION(".init.data") _Atomic int boot_hartid = -1;

// install initial page table
__SECTION(".text.entry.start")
void boot_pgtbl_init() {
    asm volatile("csrw satp, %0" : : "r"(MAKE_SATP(init_pgtbl)));
    // flush TLB
    asm volatile("sfence.vma");
}

// entry.S jumps here in supervisor mode on stack0.
// When booting from OpenSBI:
//   - We're already in S-mode
//   - hartid is passed in a0 (already saved to tp in entry.S)
//   - dtb pointer is passed in a1
__SECTION(".text.entry.start")
void start(int hartid, void *fdt_base) {
    bool is_boot_hart = false;
    // The first hart to get here becomes the boot hart
    is_boot_hart = atomic_cas(&boot_hartid, -1, hartid);

    // set up the initial page table, which maps the first 16GB of physical memory
    boot_pgtbl_init();

    // enable supervisor-mode interrupts.
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    asm volatile("or sp, sp, %0;" 
                 "or fp, fp, %0":: "r"(KERNEL_OFFSET) : "memory");

    // ask for clock interrupts.
    arch_timer_init();

    // jump to main().
    start_kernel(hartid, fdt_base, is_boot_hart);
}

// ask each hart to generate timer interrupts.
// When using OpenSBI, the firmware has already configured:
//   - menvcfg STCE bit for sstc extension
//   - mcounteren for stimecmp and time access
// We just need to set up the first timer interrupt.
__SECTION(".text.entry.start")
void arch_timer_init() {
    // calculate jiff ticks.
    // One time calculation, thus no optimization needed.
    __jiff_ticks = TIMEBASE_FREQUENCY / HZ;

    // ask for the very first timer interrupt.
    w_stimecmp(r_time() + JIFF_TICKS);
}

__SECTION(".text.entry.start")
void arch_timer_init_hart() {
    // ask for the very first timer interrupt.
    w_stimecmp(r_time() + JIFF_TICKS);
}
