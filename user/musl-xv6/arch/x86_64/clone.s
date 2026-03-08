/*
 * __clone wrapper for xv6 x86_64
 *
 * musl's pthread_create calls __clone(func, stack, flags, arg, ptid, tls, ctid)
 * C calling convention: rdi=func, rsi=stack, rdx=flags, rcx=arg, r8=ptid, r9=tls
 *                       and ctid is on the stack at 8(%rsp)
 *
 * xv6 clone takes a single pointer to struct clone_args in rdi.
 *
 * clone_args layout:
 *   offset  0: flags   (uint64)
 *   offset  8: stack   (uint64)
 *   offset 16: stack_size (uint64)
 *   offset 24: entry   (uint64)
 *   offset 32: esignal (uint64)
 *   offset 40: tls     (uint64)
 *   offset 48: ctid    (uint64)
 *   offset 56: ptid    (uint64)
 *
 * Linux x86-64 syscall convention:
 *   rcx and r11 are clobbered by SYSCALL (transaction registers).
 *   After the syscall instruction, rcx/r11 no longer hold their
 *   pre-syscall values.  This is fine because this function consumes
 *   rcx (=arg) before issuing syscall, and rcx/r11 are caller-saved
 *   in the C ABI.
 */

.global __clone
.hidden __clone
.type __clone, @function
__clone:
    /* Arguments from musl C ABI:
     *   rdi = func,  rsi = stack,  rdx = flags,
     *   rcx = arg,   r8  = ptid,   r9  = tls
     *   8(%rsp) = ctid
     */

    /* Save func and arg on the child's stack (it grows down) */
    subq $16, %rsi          /* reserve 16 bytes at top of child stack */
    movq %rdi, 0(%rsi)      /* child_stack[0] = func */
    movq %rcx, 8(%rsi)      /* child_stack[1] = arg */

    /* ctid is the 7th arg, on the stack */
    movq 8(%rsp), %rax      /* rax = ctid */

    /* Build clone_args on parent stack (64 bytes) */
    subq $64, %rsp

    movq %rdx, 0(%rsp)      /* clone_args.flags = flags */
    movq %rsi, 8(%rsp)       /* clone_args.stack = child stack */
    movq $0,   16(%rsp)      /* clone_args.stack_size = 0 */
    movq $0,   24(%rsp)      /* clone_args.entry = 0 (use func from child stack) */
    movq $0,   32(%rsp)      /* clone_args.esignal = 0 */
    movq %r9,  40(%rsp)      /* clone_args.tls = tls */
    movq %rax, 48(%rsp)      /* clone_args.ctid = ctid */
    movq %r8,  56(%rsp)      /* clone_args.ptid = ptid */

    /* Syscall: rax = SYS_clone (1), rdi = &clone_args */
    movq %rsp, %rdi
    movq $1, %rax            /* SYS_clone = 1 */
    syscall

    /* Check return value */
    testq %rax, %rax
    jz    .Lchild
    /* Parent: clean up stack and return child pid (or error) */
    addq $64, %rsp
    ret

.Lchild:
    /* Child returns here with rsp = child stack set by kernel.
     * The kernel set rsp to clone_args.stack, which has:
     *   0(%rsp) = func
     *   8(%rsp) = arg
     */
    xorq %rbp, %rbp          /* mark end of frames */
    popq %rax                 /* rax = func */
    popq %rdi                 /* rdi = arg */
    /* Ensure 16-byte stack alignment before call (x86_64 ABI).
     * musl only guarantees 8-byte alignment for the child stack.
     * callq will push 8 bytes, so RSP must be 16-aligned now. */
    andq $-16, %rsp
    callq *%rax               /* func(arg) */

    /* func returned — call exit_group */
    movq %rax, %rdi
    movq $4, %rax             /* SYS_exit_group = 4 */
    syscall
    hlt
