# Context switch for x86_64
#
#   struct context *arch_context_switch(struct context *current,
#                                       struct context *target);
#
# Save callee-saved registers into *current, then load from *target.
# Returns current (rdi) — the switched-out context pointer — in rax.
#
# struct context layout (from trapframe.h) — offsets from asm-offsets.h.

#include "asm-offsets.h"

.section .text

.global arch_context_switch
.align 16
arch_context_switch:
        # rdi = struct context *current  (save into here)
        # rsi = struct context *target   (load from here)

        # Save callee-saved registers into current context.
        # The CALL that invoked us pushed the return address onto the
        # stack, so it is already at (%rsp).  We save rsp as-is;
        # the return address is implicitly part of the stack frame.
        movq    %rsp,  CONTEXT_RSP(%rdi)
        movq    %rbx,  CONTEXT_RBX(%rdi)
        movq    %rbp,  CONTEXT_RBP(%rdi)
        movq    %r12,  CONTEXT_R12(%rdi)
        movq    %r13,  CONTEXT_R13(%rdi)
        movq    %r14,  CONTEXT_R14(%rdi)
        movq    %r15,  CONTEXT_R15(%rdi)

        # Return value: the current context pointer (rdi)
        movq    %rdi,  %rax

        # Load callee-saved registers from target context
        movq    CONTEXT_RBX(%rsi), %rbx
        movq    CONTEXT_RBP(%rsi), %rbp
        movq    CONTEXT_R12(%rsi), %r12
        movq    CONTEXT_R13(%rsi), %r13
        movq    CONTEXT_R14(%rsi), %r14
        movq    CONTEXT_R15(%rsi), %r15
        movq    CONTEXT_RSP(%rsi), %rsp        # switch stacks
        # For a resumed thread the return address is already at (%rsp)
        # (saved by the CALL when that thread previously switched out).
        # For a new thread arch_context_init placed the entry address
        # at (%rsp).  Either way, ret finds the right target.
        ret

# void __switch_noreturn(uint64 sp, uint64 s0, sw_noret_cb_t addr);
# Switch to a new stack and jump to the callback.  Never returns.
# rdi = new sp, rsi = rbp (unused/debug), rdx = callback address
# callback prototype: void callback(uint64 old_sp, uint64 old_rbp);
.global __switch_noreturn
.align 16
__switch_noreturn:
        movq    %rsp, %rax              # save old sp
        movq    %rdi, %rsp              # switch to new stack
        movq    %rax, %rdi              # pass old sp as arg1
        # rsi already has s0/rbp as arg2
        jmpq    *%rdx                   # jump to callback (never returns)
    ret