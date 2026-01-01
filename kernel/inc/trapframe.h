#ifndef __KERNEL_TRAPFRAME_H
#define __KERNEL_TRAPFRAME_H

#include "types.h"

struct proc;

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
struct trapframe {
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
  /*  24 */ uint64 sepc;           // saved user program counter
  /* 288 */ uint64 scause;      // saved scause
  /* 296 */ uint64 stval;        // saved stval
  /* 312 */ uint64 sstatus;      // saved sstatus

// The following fields only applicable for usertrap and usertrapret
  /* 328 */ uint64 irq_sp;       // saved interrupt stack pointer
  /* 336 */ uint64 irq_entry;    // saved interrupt entry point
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  64 */ uint64 tp;
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  56 */ uint64 gp;
  /* 320 */ uint64 kernel_gp;    // saved kernel gp
};


struct ktrapframe {
  /*   0 */ uint64 ra;
  /*   8 */ uint64 sp;
  /*  16 */ uint64 s0;
  /*  32 */ uint64 t0;
  /*  40 */ uint64 t1;
  /*  48 */ uint64 t2;
  /*  72 */ uint64 a0;
  /*  80 */ uint64 a1;
  /*  88 */ uint64 a2;
  /*  96 */ uint64 a3;
  /* 104 */ uint64 a4;
  /* 112 */ uint64 a5;
  /* 120 */ uint64 a6;
  /* 128 */ uint64 a7;
  /* 216 */ uint64 t3;
  /* 224 */ uint64 t4;
  /* 232 */ uint64 t5;
  /* 240 */ uint64 t6;
  /*  56 */ uint64 sepc;
  /*  64 */ uint64 sstatus;
  /* 136 */ uint64 scause;
  /* 144 */ uint64 stval;
  /* 152 */ uint64 stvec;
  /* 160 */ uint64 sscratch;
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
} __attribute__((aligned(8)));

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  void **intr_stacks;         // Top of interrupt stack for each hart.
  uint64 intr_sp;             // Saved sp value for interrupt.
  int intr_depth;             // Depth of nested interruption.
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

#endif /* __KERNEL_TRAPFRAME_H */
