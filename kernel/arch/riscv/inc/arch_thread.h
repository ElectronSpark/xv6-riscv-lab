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

/* ── Kernel stack layout ── */

#include "compiler.h"   /* CACHELINE_MASK */

/**
 * Flags for arch_kstack_arrange().
 */
#define KSTACK_ARRANGE_FLAGS_TF  0x1  /* place utrapframe */
#define KSTACK_ARRANGE_FLAGS_ALL (KSTACK_ARRANGE_FLAGS_TF)

/**
 * struct kstack_layout - result of laying out PCB structures on
 *                        a kernel stack.
 */
struct kstack_layout {
    struct thread       *thread;
    struct utrapframe   *trapframe;
    struct sched_entity *sched_entity;
    uint64               ksp;
};

/**
 * arch_kstack_arrange - Compute the layout of PCB structures at the
 *                       top of a kernel stack (architecture-specific).
 *
 * On RISC-V the structures are placed directly at the top of the
 * stack with 8-byte alignment.
 */
static inline struct kstack_layout
arch_kstack_arrange(void *kstack, size_t kstack_size, uint64 flags) {
    struct kstack_layout lay;

    /* struct thread at the very top */
    lay.thread = (struct thread *)(
        (char *)kstack + kstack_size - sizeof(struct thread));
    uint64 next = (uint64)lay.thread;

    /* Optional utrapframe below thread */
    lay.trapframe = NULL;
    if (flags & KSTACK_ARRANGE_FLAGS_TF) {
        next = (uint64)lay.thread - sizeof(struct utrapframe) - 16;
        next &= ~0x7UL;
        lay.trapframe = (struct utrapframe *)next;
    }

    /* sched_entity — cache-line aligned */
    next = next - sizeof(struct sched_entity);
    next &= ~CACHELINE_MASK;
    lay.sched_entity = (struct sched_entity *)next;

    /* Kernel stack pointer below everything */
    lay.ksp = (next - 16) & ARCH_KSP_ALIGN_MASK;

    return lay;
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
