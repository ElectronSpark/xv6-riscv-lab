#ifndef __KERNEL_PERCPU_TYPES_H
#define __KERNEL_PERCPU_TYPES_H

#include "types.h"

struct proc;

// Per-CPU state.
struct cpu_local {
    struct proc *proc;          // The process running on this cpu, or null.
    struct proc *idle_proc;     // The idle process for this cpu.
    void **intr_stacks;         // Top of interrupt stack for each hart.
    uint64 intr_sp;             // Saved sp value for interrupt.
    int intr_depth;             // Depth of nested interruption or exception.
    int noff;                   // Depth of push_off() nesting.
    int spin_depth;             // Depth of spinlock nesting.
    int intena;                 // Were interrupts enabled before push_off()?
    uint64 flags;               // CPU flags
    uint64 rcu_timestamp;       // RCU timestamp - updated before context switch
}  __attribute__((aligned(64)));


#endif        /* __KERNEL_PERCPU_TYPES_H */
