#ifndef __KERNEL_X86_64_ARCH_THREAD_H
#define __KERNEL_X86_64_ARCH_THREAD_H

/**
 * @file arch_thread.h
 * @brief x86_64 architecture-specific thread helpers.
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

/** x86_64 System V ABI requires 16-byte stack alignment. */
#define ARCH_KSP_ALIGN_MASK (~0xFUL)

/* ── Initial context for a newly created thread ── */

/**
 * arch_context_init - Set the initial context so arch_context_switch
 *                     lands at @entry with stack pointer @ksp.
 *
 * On x86_64 the return address lives on the stack (ret pops it),
 * so we push @entry onto the kernel stack and record the adjusted
 * rsp in the context.  When arch_context_switch restores rsp and
 * executes ret, it jumps straight to @entry.
 */
static inline void arch_context_init(struct context *ctx,
                                     uint64 entry, uint64 ksp) {
    ksp -= 8;
    *(uint64 *)ksp = entry;   /* fake return address for ret */
    ctx->rsp = ksp;
    ctx->rbp = 0;             /* frame pointer */
}

/**
 * arch_context_set_entry - Override the entry point of an already-
 *                          initialised context (before first switch).
 *
 * On x86_64 the entry sits on the stack at *(ctx->rsp).
 */
static inline void arch_context_set_entry(struct context *ctx,
                                          uint64 entry) {
    *(uint64 *)ctx->rsp = entry;
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
        tf->trapframe.rip = entry;

    if (stack != 0) {
        uint64 stack_top = (stack + stack_size) & ~0xFUL;
        tf->trapframe.rsp = stack_top;
    }

    /* Fork / clone returns 0 in the child (rax on x86_64). */
    tf->trapframe.rax = 0;

    /* CLONE_SETTLS: will set FS-base when returning to user mode. */
    if (clone_flags & CLONE_SETTLS)
        tf->tp = tls;
}

#endif /* __KERNEL_X86_64_ARCH_THREAD_H */
