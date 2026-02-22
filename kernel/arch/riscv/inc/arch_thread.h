#ifndef __KERNEL_RISCV_ARCH_THREAD_H
#define __KERNEL_RISCV_ARCH_THREAD_H

/**
 * @file arch_thread.h
 * @brief RISC-V architecture-specific thread helpers.
 *
 * Provides the small set of arch-dependent operations that the shared
 * thread / clone code needs:
 *   - kernel-stack-pointer alignment
 *   - initial context register setup
 *   - child register adjustments after clone/fork
 */

#include "trapframe.h"
#include "types.h"
#include "clone_flags.h"

/* ── Kernel-stack pointer alignment ── */

/** RISC-V ABI requires 8-byte (doubleword) stack alignment. */
#define ARCH_KSP_ALIGN_MASK (~0x7UL)

/* ── Initial context for a newly created thread ── */

/**
 * arch_context_init - Set the initial context so arch_context_switch
 *                     lands at @entry with stack pointer @ksp.
 */
static inline void arch_context_init(struct context *ctx,
                                     uint64 entry, uint64 ksp) {
    ctx->ra = entry;
    ctx->sp = ksp;
    ctx->s0 = 0;   /* frame pointer */
}

/**
 * arch_context_set_entry - Override the entry point of an already-
 *                          initialised context (before first switch).
 *
 * On RISC-V the entry is stored in ctx->ra.
 */
static inline void arch_context_set_entry(struct context *ctx,
                                          uint64 entry) {
    ctx->ra = entry;
}

/* ── Child register adjustments for clone/fork ── */

/**
 * arch_clone_child_regs - Apply architecture-specific register
 *                         adjustments to a freshly cloned child.
 *
 * Called after the parent's trapframe has been copied wholesale
 * into the child.  Sets the child's return value to 0, and
 * optionally overrides entry point, stack, and TLS.
 */
static inline void arch_clone_child_regs(struct utrapframe *tf,
                                         uint64 clone_flags,
                                         uint64 entry,
                                         uint64 stack, uint64 stack_size,
                                         uint64 tls) {
    if (entry != 0)
        tf->trapframe.sepc = entry;

    if (stack != 0) {
        uint64 stack_top = (stack + stack_size) & ~0xFUL;
        tf->trapframe.sp = stack_top;
    }

    /* Fork / clone returns 0 in the child. */
    tf->trapframe.a0 = 0;

    if (clone_flags & CLONE_SETTLS)
        tf->tp = tls;
}

#endif /* __KERNEL_RISCV_ARCH_THREAD_H */
