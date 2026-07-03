#ifndef __KERNEL_PERCPU_TYPES_H
#define __KERNEL_PERCPU_TYPES_H

#include "compiler.h"
#include "types.h"

struct proc;
struct thread;

// Per-CPU state.
struct cpu_local {
    struct thread *proc;        // The thread running on this cpu, or null.
    struct thread *idle_thread; // The idle process for this cpu.
    void **intr_stacks;         // Top of interrupt stack for each hart.
    uint64 intr_sp;             // Saved sp value for interrupt.
    int intr_depth;             // Depth of nested interruption or exception.
    int noff;                   // Depth of push_off() nesting.
    int spin_depth;             // Depth of spinlock nesting.
    int intena;                 // Were interrupts enabled before push_off()?
    uint64 flags;               // CPU flags
    uint64 rcu_timestamp;       // RCU timestamp - updated before context switch
    int rcu_read_lock_nesting;  // Authoritative RCU read-side nesting on CPU
    int __pad_rcu;              // keep following 64-bit fields aligned
    uint64 syscall_scratch;     // SWAPGS scratch (%gs:64 on SYSCALL entry)
    uint64 syscall_kstack_top;  // kernel stack top for SYSCALL entry (%gs:72)
    uint64 syscall_trapframe;   // higher-half trapframe VA for SYSCALL entry
    int fpu_owner_tid;          // TID of thread whose FP state is in HW (0 = none)
    int __pad0;
    uint64 intr_kstack_top;     // kernel stack top for IDT entry
    uint64 fpu_seq;             // FPU ownership sequence number
    uint64 busy_ticks;          // ticks spent running non-idle threads
    uint64 total_ticks;         // total timer ticks on this CPU
    uint64 util_1s;             // CPU utilization over last 1s (FSHIFT=11 fp, FIXED_1=100%)
    uint16 asid_gen;          // last-seen ASID generation on this CPU
    uint16 __pad1;
    uint32 __pad2;
    uint64 user_fs_base;      // Last MSR_FS_BASE value installed on this CPU
    int user_fs_base_valid;   // Avoid sentinel collisions with user values
    int __pad_user_fs_base;
} __ALIGNED_CACHELINE;

#endif /* __KERNEL_PERCPU_TYPES_H */
