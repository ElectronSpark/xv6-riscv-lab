.text

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
 */

.align 4096
.globl trampoline
trampoline:

.align 16
.globl userret
userret:
    /* Preserve user CR3 across frame setup. rdi keeps trapframe VA. */
    movq %rsi, %r13               /* r13 = user CR3 */

    /* Switch to user page table while still executing in trampoline page. */
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
    pushq 0xA0(%rdi)              /* tf->rsp */
    pushq 0x98(%rdi)              /* tf->rflags */
    pushq $0x2b                   /* SEG_UCODE | DPL_USER */
    pushq 0x88(%rdi)              /* tf->rip */

    /* Restore GPRs from trapframe. */
    movq 0x00(%rdi), %r15
    movq 0x08(%rdi), %r14
    movq 0x10(%rdi), %r13
    movq 0x18(%rdi), %r12
    movq 0x20(%rdi), %r11
    movq 0x28(%rdi), %r10
    movq 0x30(%rdi), %r9
    movq 0x38(%rdi), %r8
    movq 0x40(%rdi), %rbp
    movq 0x50(%rdi), %rsi
    movq 0x58(%rdi), %rdx
    movq 0x60(%rdi), %rcx
    movq 0x68(%rdi), %rbx
    movq 0x70(%rdi), %rax
    movq 0x48(%rdi), %rdi

    swapgs
    iretq
