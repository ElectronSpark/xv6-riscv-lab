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
    uint64 rsp0 = ksp - 16;
    *(uint64 *)rsp0 = entry;         /* ret target for first switch */
    *(uint64 *)(rsp0 + 8) = 0;       /* dummy return address if entry returns */
    ctx->rsp = rsp0;
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
 * On x86_64 the structures are placed at the top of the stack with
 * 16-byte alignment.  Identical to RISC-V for now, but kept separate
 * so that platform-specific adjustments (e.g. reserved IST area or
 * red-zone avoidance) can be added without touching shared code.
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
        tf->trapframe.rip = entry;

    if (stack != 0) {
        /* When stack_size != 0: stack is the base, compute top.
         * When stack_size == 0: stack IS the SP directly (Linux convention
         * used by musl's __clone). Do not re-align — caller already did. */
        uint64 stack_top = stack_size ? ((stack + stack_size) & ~0xFUL) : stack;
        tf->trapframe.rsp = stack_top;
    }

    /* Fork / clone returns 0 in the child (rax on x86_64). */
    tf->trapframe.rax = 0;

    /* CLONE_SETTLS: will set FS-base when returning to user mode. */
    if (clone_flags & CLONE_SETTLS)
        tf->tp = tls;
}

/* ── Trapframe return-value accessors (x86_64 System V ABI) ── */

static inline void arch_tf_set_ret(struct utrapframe *tf, uint64 v) {
    tf->trapframe.rax = v;
}
static inline uint64 arch_tf_get_ret(struct utrapframe *tf) {
    return tf->trapframe.rax;
}

/* ── Trapframe Linux syscall-argument accessors (x86_64 syscall ABI) ── */

static inline void arch_tf_set_arg0(struct utrapframe *tf, uint64 v) {
    tf->trapframe.rdi = v;
}
static inline void arch_tf_set_arg1(struct utrapframe *tf, uint64 v) {
    tf->trapframe.rsi = v;
}
static inline void arch_tf_set_arg2(struct utrapframe *tf, uint64 v) {
    tf->trapframe.rdx = v;
}
static inline void arch_tf_set_arg3(struct utrapframe *tf, uint64 v) {
    tf->trapframe.r10 = v;
}
static inline void arch_tf_set_arg4(struct utrapframe *tf, uint64 v) {
    tf->trapframe.r8 = v;
}
static inline void arch_tf_set_arg5(struct utrapframe *tf, uint64 v) {
    tf->trapframe.r9 = v;
}

static inline uint64 arch_tf_get_arg0(struct utrapframe *tf) {
    return tf->trapframe.rdi;
}
static inline uint64 arch_tf_get_arg1(struct utrapframe *tf) {
    return tf->trapframe.rsi;
}
static inline uint64 arch_tf_get_arg2(struct utrapframe *tf) {
    return tf->trapframe.rdx;
}
static inline uint64 arch_tf_get_arg3(struct utrapframe *tf) {
    return tf->trapframe.rcx;
}
static inline uint64 arch_tf_get_arg4(struct utrapframe *tf) {
    return tf->trapframe.r8;
}
static inline uint64 arch_tf_get_arg5(struct utrapframe *tf) {
    return tf->trapframe.r9;
}

/**
 * arch_tf_set_exec_args - Set up the trapframe registers for exec.
 *
 * On x86_64 the syscall return value is in RAX but the first function
 * argument is in RDI (a different register), so we must explicitly
 * place argc into RDI and argv into RSI.
 */
static inline void arch_tf_set_exec_args(struct utrapframe *tf,
                                         uint64 argc, uint64 argv) {
    arch_tf_set_arg0(tf, argc);
    arch_tf_set_arg1(tf, argv);
    /*
     * AMD64 process entry uses RDX for the optional rtld_fini callback.
     * A static executable has no dynamic loader to provide one, so it must
     * start as NULL.  Leaving the execve syscall's envp argument here makes
     * glibc call a bogus function pointer during exit/fini.
     */
    arch_tf_set_arg2(tf, 0);
    tf->trapframe.rcx = 0;
    tf->trapframe.r8 = 0;
    tf->trapframe.r9 = 0;
    tf->trapframe.r10 = 0;
    tf->trapframe.r11 = 0;
}

#endif /* __KERNEL_X86_64_ARCH_THREAD_H */
