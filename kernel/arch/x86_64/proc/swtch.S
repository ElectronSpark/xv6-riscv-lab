# Context switch for x86_64
#
#   struct context *arch_context_switch(struct context *current,
#                                       struct context *target);
#
# Save callee-saved registers into *current, then load from *target.
# Returns current (rdi) — the switched-out context pointer — in rax.
#
# struct context layout (from trapframe.h):
#   0x00  ra   (return address)
#   0x08  rsp
#   0x10  rbx
#   0x18  rbp
#   0x20  r12
#   0x28  r13
#   0x30  r14
#   0x38  r15

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
        movq    %rsp,  0x08(%rdi)       # ctx->rsp
        movq    %rbx,  0x10(%rdi)       # ctx->rbx
        movq    %rbp,  0x18(%rdi)       # ctx->rbp
        movq    %r12,  0x20(%rdi)       # ctx->r12
        movq    %r13,  0x28(%rdi)       # ctx->r13
        movq    %r14,  0x30(%rdi)       # ctx->r14
        movq    %r15,  0x38(%rdi)       # ctx->r15

        # Return value: the current context pointer (rdi)
        movq    %rdi,  %rax

        # Load callee-saved registers from target context
        movq    0x10(%rsi), %rbx        # ctx->rbx
        movq    0x18(%rsi), %rbp        # ctx->rbp
        movq    0x20(%rsi), %r12        # ctx->r12
        movq    0x28(%rsi), %r13        # ctx->r13
        movq    0x30(%rsi), %r14        # ctx->r14
        movq    0x38(%rsi), %r15        # ctx->r15
        movq    0x08(%rsi), %rsp        # ctx->rsp (switch stacks)
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