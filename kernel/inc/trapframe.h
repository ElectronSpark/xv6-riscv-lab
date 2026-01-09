#ifndef __KERNEL_TRAPFRAME_H
#define __KERNEL_TRAPFRAME_H

#include "types.h"

struct trapframe {
    uint64 ra;
    uint64 sp;
    uint64 s0;
    uint64 t0;
    uint64 t1;
    uint64 t2;
    uint64 a0;
    uint64 a1;
    uint64 a2;
    uint64 a3;
    uint64 a4;
    uint64 a5;
    uint64 a6;
    uint64 a7;
    uint64 t3;
    uint64 t4;
    uint64 t5;
    uint64 t6;
    uint64 sepc;
    uint64 sstatus;
    uint64 scause;
    uint64 stval;
    uint64 stvec;
} __attribute__((aligned(8)));

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct utrapframe {
    struct trapframe trapframe;

    // The following fields only applicable for usertrap and usertrapret
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;

    uint64 irq_sp;       // saved interrupt stack pointer
    uint64 irq_entry;    // saved interrupt entry point
    uint64 kernel_satp;   // kernel page table
    uint64 kernel_sp;     // top of process's kernel stack
    uint64 kernel_trap;   // usertrap()
    uint64 tp;
    uint64 kernel_hartid; // saved kernel tp
    uint64 gp;
    uint64 kernel_gp;    // saved kernel gp
} __attribute__((aligned(8)));


// Saved registers for kernel context switches.
struct context {
    uint64 ra;
    uint64 sp;

    // callee-saved
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
} __attribute__((aligned(64)));

#endif /* __KERNEL_TRAPFRAME_H */
