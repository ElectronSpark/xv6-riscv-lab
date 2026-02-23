/*
 * sig_trampoline.S — x86_64 signal trampoline
 *
 * Mapped at SIG_TRAMPOLINE in every user page table (PTE_R | PTE_U,
 * no PTE_NX).  After a user signal handler returns, execution falls
 * through here to invoke sys_sigreturn.
 *
 * On entry (set up by push_sigframe in trap.c):
 *   RIP = handler address   (signal handler)
 *   RSP points to:
 *       [RSP]   = return address → sig_trampoline (here)
 *       [RSP+8] = ucontext, siginfo, etc. on the stack
 *
 * When the handler executes RET it pops the return address and
 * lands here.  We then issue syscall(SYS_sigreturn).
 */

#include "syscall.h"

.section sig_trampsec, "ax", @progbits
.global sig_trampoline
.type sig_trampoline, @function
.align 16
sig_trampoline:
    /* SYS_sigreturn = 71 (defined in kernel/inc/syscall.h) */
    movq    $SYS_sigreturn, %rax
    syscall
    /* Should never return — trap if it does. */
    ud2
.size sig_trampoline, . - sig_trampoline
