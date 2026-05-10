.text

#include "asm-offsets.h"

/*
 * x86_64 user return trampoline.
 *
 * Like the RISC-V trampoline path, this routine is mapped at a fixed
 * high virtual address (TRAMPOLINE) in both kernel and user page tables.
 * That allows us to switch CR3 to the user page table and continue
 * executing safely while restoring registers and returning to user mode.
 *
 * Calling convention:
 *   rdi = trapframe virtual address in shared TRAPFRAME window
 *   rsi = user pagetable (CR3 value)
 *   rdx = user GS base
 *   rcx = kernel GS base (per-CPU struct)
 */

.align 4096
.globl trampoline
trampoline:

.align 16
.globl userret
userret:
    /* Preserve return state across frame setup. rdi keeps trapframe VA. */
    movq %rsi, %r13               /* r13 = user CR3 (with PCID + noflush bit) */
    movq %rdx, %r14               /* r14 = user GS base */
    movq %rcx, %r15               /* r15 = kernel GS base */

    /* Switch to user page table while still executing in trampoline page.
     * With PCID enabled, the noflush bit (63) in %r13 preserves TLB
     * entries from other PCIDs — no full TLB flush on context switch. */
    movq %r13, %cr3

    /*
     * Use the trapframe page itself as temporary ring0 stack.
     * Kernel low VA stack is not mapped in user page tables.
     */
    movq %rdi, %rsp
    andq $-4096, %rsp
    addq $4096, %rsp

    /* Build iret frame: SS, RSP, RFLAGS, CS, RIP */
    pushq $0x23                   /* SEG_UDATA | DPL_USER */
    pushq TRAPFRAME_RSP(%rdi)     /* tf->rsp */
    pushq TRAPFRAME_RFLAGS(%rdi)  /* tf->rflags */
    pushq $0x2b                   /* SEG_UCODE | DPL_USER */
    pushq TRAPFRAME_RIP(%rdi)     /* tf->rip */

    /*
     * Install GS bases as the last privileged work before restoring user GPRs.
     * usertrapret() keeps GS_BASE pointing at per-CPU data while it runs C;
     * doing the user GS switch here avoids kernel-mode traps seeing user GS.
     * After this point, do not use %gs before iretq.
     */
    movl $0xC0000101, %ecx        /* MSR_GS_BASE */
    movq %r14, %rax
    movq %rax, %rdx
    shrq $32, %rdx
    wrmsr

    movl $0xC0000102, %ecx        /* MSR_KERNEL_GS_BASE */
    movq %r15, %rax
    movq %rax, %rdx
    shrq $32, %rdx
    wrmsr

    /*
     * Restore GPRs from trapframe.
     *
     * For SYSCALL returns: rcx and r11 are "clobbered" per the Linux
     * syscall convention.  Callers must save them before SYSCALL.
     * We still restore them here so the IDT trap path (which shares
     * this code) preserves the original user rcx/r11 correctly.
     * On the SYSCALL path the restored values are the CPU-set
     * transition state (user RIP / user RFLAGS), not originals.
     */
    movq TRAPFRAME_R15(%rdi), %r15
    movq TRAPFRAME_R14(%rdi), %r14
    movq TRAPFRAME_R13(%rdi), %r13
    movq TRAPFRAME_R12(%rdi), %r12
    movq TRAPFRAME_R11(%rdi), %r11          /* clobbered on SYSCALL path      */
    movq TRAPFRAME_R10(%rdi), %r10
    movq TRAPFRAME_R9(%rdi), %r9
    movq TRAPFRAME_R8(%rdi), %r8
    movq TRAPFRAME_RBP(%rdi), %rbp
    movq TRAPFRAME_RSI(%rdi), %rsi
    movq TRAPFRAME_RDX(%rdi), %rdx
    movq TRAPFRAME_RCX(%rdi), %rcx          /* clobbered on SYSCALL path      */
    movq TRAPFRAME_RBX(%rdi), %rbx
    movq TRAPFRAME_RAX(%rdi), %rax
    movq TRAPFRAME_RDI(%rdi), %rdi

    iretq
