/*
 * __clone wrapper for xv6
 *
 * musl's pthread_create calls __clone(func, stack, flags, arg, ptid, tls, ctid)
 * Linux clone takes different register args. xv6 takes a single pointer to
 * struct clone_args.
 *
 * This wrapper bridges the two conventions:
 *   musl:  __clone(func, stack, flags, arg, ...)
 *   xv6:   ecall with a0 = &clone_args
 *
 * The child thread starts by calling func(arg).
 */

.global __clone
.hidden __clone
.type __clone, %function
__clone:
    /*
     * Arguments from musl:
     *   a0 = func     (entry point for child thread)
     *   a1 = stack     (child stack base)
     *   a2 = flags     (clone flags)
     *   a3 = arg       (argument to func — stored on child stack)
     *   a4 = ptid      (parent tid pointer)
     *   a5 = tls       (TLS pointer)
     *   a6 = ctid      (child tid pointer) — on the stack at sp+0
     *
     * xv6 clone_args struct layout:
     *   offset 0:  flags (uint64)
     *   offset 8:  stack (uint64)
     *   offset 16: stack_size (uint64)
     *   offset 24: entry (uint64)
     *   offset 32: esignal (uint64)
     *   offset 40: tls (uint64)
     *   offset 48: ctid (uint64)
     *   offset 56: ptid (uint64)
     */

    /* Save func and arg on the child's stack (stack grows down) */
    addi a1, a1, -16       /* reserve 16 bytes at top of child stack */
    sd   a0, 0(a1)         /* child_stack[0] = func */
    sd   a3, 8(a1)         /* child_stack[1] = arg */

    /* ctid is already in a6 from the calling convention (7th arg) */

    /* Build clone_args on parent stack */
    addi sp, sp, -64       /* allocate 64 bytes for clone_args */

    sd   a2, 0(sp)         /* clone_args.flags = flags */
    sd   a1, 8(sp)         /* clone_args.stack = child stack (after func/arg) */
    li   t0, 0             /* We don't track stack_size here */
    sd   t0, 16(sp)        /* clone_args.stack_size = 0 */

    /* Entry point: __clone_child_entry (child will pop func/arg from its stack) */
    lla  t0, __clone_child_entry
    sd   t0, 24(sp)        /* clone_args.entry = __clone_child_entry */
    li   t0, 17            /* SIGCHLD */
    sd   t0, 32(sp)        /* clone_args.esignal = SIGCHLD */
    sd   a5, 40(sp)        /* clone_args.tls = tls */
    sd   a6, 48(sp)        /* clone_args.ctid = ctid */
    sd   a4, 56(sp)        /* clone_args.ptid = ptid */

    /* syscall: clone(clone_args) */
    mv   a0, sp            /* a0 = &clone_args */
    li   a7, 1             /* SYS_clone = 1 */
    ecall

    /* Parent returns here with child PID in a0 (or negative error) */
    addi sp, sp, 64        /* clean up clone_args */
    ret

/*
 * Child thread entry point.
 * The child's sp points to a frame where:
 *   sp[0] = func
 *   sp[8] = arg
 * xv6 fork returns 0 in a0 for child.
 */
.hidden __clone_child_entry
__clone_child_entry:
    ld   t0, 0(sp)         /* t0 = func */
    ld   a0, 8(sp)         /* a0 = arg */
    addi sp, sp, 16        /* pop func + arg */
    jalr t0                /* call func(arg) */

    /* func returned; exit thread */
    mv   a0, zero          /* exit status 0 */
    li   a7, 3             /* SYS_exit = 3 */
    ecall
    unimp                  /* should never reach here */
