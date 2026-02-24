/*
 * trapvec.S — x86_64 IDT vector entry points.
 *
 * Each vector gets a small stub that:
 *   1. Pushes a dummy error code (0) if the CPU doesn't push one.
 *   2. Pushes the vector number.
 *   3. Jumps to the common handler (alltraps) which saves all GPRs,
 *      calls the C handler x86_trap_handler(struct trapframe *),
 *      restores everything and does IRETQ.
 *
 * Vectors that push an error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30.
 * All others get a dummy 0 pushed.
 *
 * Build a table of 256 entry-point addresses (vectors[]) so that
 * trap.c can fill the IDT at runtime.
 */

.text

/*
 * Macros to generate vector stubs.
 *
 * TRAP_NOERR(n) — CPU does NOT push an error code for vector n.
 * TRAP_ERR(n)   — CPU pushes an error code for vector n.
 */
.macro TRAP_NOERR n
.align 16
.globl vector\n
vector\n:
    pushq $0            /* dummy error code */
    pushq $\n           /* vector number    */
    jmp alltraps
.endm

.macro TRAP_ERR n
.align 16
.globl vector\n
vector\n:
    /* error code already on stack */
    pushq $\n           /* vector number    */
    jmp alltraps
.endm

/* ── CPU exception vectors 0-31 ── */
TRAP_NOERR  0       /* #DE  Divide Error */
TRAP_NOERR  1       /* #DB  Debug */
TRAP_NOERR  2       /* NMI */
TRAP_NOERR  3       /* #BP  Breakpoint */
TRAP_NOERR  4       /* #OF  Overflow */
TRAP_NOERR  5       /* #BR  Bound Range */
TRAP_NOERR  6       /* #UD  Invalid Opcode */
TRAP_NOERR  7       /* #NM  Device Not Available */
TRAP_ERR    8       /* #DF  Double Fault */
TRAP_NOERR  9       /* (reserved) Coprocessor Segment Overrun */
TRAP_ERR   10       /* #TS  Invalid TSS */
TRAP_ERR   11       /* #NP  Segment Not Present */
TRAP_ERR   12       /* #SS  Stack-Segment Fault */
TRAP_ERR   13       /* #GP  General Protection */
TRAP_ERR   14       /* #PF  Page Fault */
TRAP_NOERR 15       /* (reserved) */
TRAP_NOERR 16       /* #MF  x87 FP Exception */
TRAP_ERR   17       /* #AC  Alignment Check */
TRAP_NOERR 18       /* #MC  Machine Check */
TRAP_NOERR 19       /* #XM  SIMD FP Exception */
TRAP_NOERR 20       /* #VE  Virtualization Exception */
TRAP_ERR   21       /* #CP  Control Protection */
TRAP_NOERR 22       /* (reserved) */
TRAP_NOERR 23
TRAP_NOERR 24
TRAP_NOERR 25
TRAP_NOERR 26
TRAP_NOERR 27
TRAP_NOERR 28
TRAP_ERR   29       /* #SX  Security Exception */
TRAP_ERR   30       /* #HV  VMM Communication */
TRAP_NOERR 31       /* (reserved) */

/* ── IRQ / software vectors 32-255 ── */
/*
 * Generate vectors 32-255 with a loop.  None of them push error codes.
 * Use .altmacro so that %i evaluates the .set variable's current value.
 */
.altmacro
.set i, 32
.rept 224
    TRAP_NOERR %i
    .set i, i + 1
.endr
.noaltmacro

/* ──────────────────────────────────────────────────────────────
 *  alltraps — common entry.  Saves all GPRs, calls C handler,
 *  restores state and returns via IRETQ.
 *
 *  NO SWAPGS.  Instead, the kernel page table (CR3) is stored in
 *  trampoline_ksatp below (analogous to RISC-V trampoline_ksatp).
 *  It is accessed RIP-relative, which works from the high alias
 *  because the data lives in the same pages as the code.
 *
 *  On IDT entry the CPU has already switched RSP to TSS.RSP0 or
 *  IST, so we are on a known kernel stack.  We only need to load
 *  the kernel CR3.
 * ────────────────────────────────────────────────────────────── */

.align 16
.globl alltraps
alltraps:
    /*
     * At this point the IST1 stack looks like:
     *   [RSP+0x30]  SS
     *   [RSP+0x28]  RSP (user)
     *   [RSP+0x20]  RFLAGS
     *   [RSP+0x18]  CS
     *   [RSP+0x10]  RIP
     *   [RSP+0x08]  error code     (pushed by CPU or stub)
     *   [RSP+0x00]  vector number  (pushed by stub)
     *
     * All IDT vectors use IST=1, so the CPU always loads RSP from
     * TSS.IST1 (a per-CPU stack mapped in every page table).
     *
     * Step 1: switch to the kernel page table (same as before).
     */
    pushq %rax
    pushq %rbx
    movq  %cr3, %rax
    leaq  trampoline_ksatp(%rip), %rbx
    movq  (%rbx), %rbx            /* rbx = stored kernel CR3       */
    cmpq  %rax, %rbx
    je    1f
    movq  %rbx, %cr3              /* switch to kernel page table   */
1:
    popq  %rbx
    popq  %rax

    /*
     * Step 2: for user-mode traps, pivot RSP from the shared IST
     * stack to the per-thread kernel stack (kstack).
     *
     * WHY: with IST=1 every interrupt lands on the same per-CPU IST
     * stack.  If two threads context-switch while their call frames
     * are on the IST stack, the second thread's frames overwrite the
     * first's, corrupting saved state.  By moving to a per-thread
     * stack immediately after CR3 swap (while interrupts are still
     * disabled by GATE_INTERRUPT), all subsequent C code — including
     * scheduler context switches — runs on a private stack.
     *
     * Kernel-mode traps skip the pivot and stay on the current stack
     * (which is either the IST for brief ISR handling, or the
     * already-private kstack of the running kernel thread).
     */
    testq $3, 24(%rsp)            /* CS & 3 → RPL                  */
    jz    .Lkstack_done           /* kernel mode → no switch needed */

    /* ── FROM USER MODE ── copy interrupt frame to kstack ──
     *
     * IST layout at this point (RSP-relative):
     *   +0  vec   +8  err   +16 rip   +24 cs
     *   +32 rflags   +40 user_rsp   +48 ss
     *
     * Save rax & rbx as scratch on the IST stack, then copy the
     * 7 qwords into the per-thread kernel stack whose top is stored
     * in the RIP-relative variable trampoline_kstack (set by
     * usertrapret before every user return).
     */
    pushq %rax                    /* save rax on IST               */
    pushq %rbx                    /* save rbx on IST               */
    /* IST: rbx(+0) rax(+8) vec(+16) err(+24) rip(+32) cs(+40)
     *       rflags(+48) user_rsp(+56) ss(+64)                     */

    leaq  trampoline_kstack(%rip), %rax
    movq  (%rax), %rax            /* rax = kstack top (= p->ksp)   */

    /* Copy 7 values from IST to kstack[-56 .. kstack-8].          */
    movq  64(%rsp), %rbx
    movq  %rbx, -8(%rax)          /* ss                            */
    movq  56(%rsp), %rbx
    movq  %rbx, -16(%rax)         /* user rsp                      */
    movq  48(%rsp), %rbx
    movq  %rbx, -24(%rax)         /* rflags                        */
    movq  40(%rsp), %rbx
    movq  %rbx, -32(%rax)         /* cs                            */
    movq  32(%rsp), %rbx
    movq  %rbx, -40(%rax)         /* rip                           */
    movq  24(%rsp), %rbx
    movq  %rbx, -48(%rax)         /* err                           */
    movq  16(%rsp), %rbx
    movq  %rbx, -56(%rax)         /* vec                           */

    subq  $56, %rax               /* rax = kstack - 56             */

    /* Restore rbx, then atomically swap rax (kstack-56) with the
     * saved original rax on the IST stack, and finally load RSP
     * from that IST slot to complete the pivot.                    */
    popq  %rbx                    /* restore original rbx          */
    xchgq %rax, (%rsp)           /* rax ← orig rax; (%rsp) ← kstack-56 */
    movq  (%rsp), %rsp           /* RSP = kstack - 56  (PIVOT!)   */

    /* RSP now points into the per-thread kstack with the 7-qword
     * interrupt frame.  rax/rbx hold their original user values.  */

.Lkstack_done:
    /* Step 3: push all 15 GPRs to build a full struct trapframe.  */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* RDI = pointer to struct trapframe (first C argument) */
    movq %rsp, %rdi

    /* Ensure direction flag is clear (ABI requirement) */
    cld

    /* Call C trap handler via absolute target (trapvec runs from high alias). */
    movabsq $x86_trap_handler, %rax
    call *%rax

    /* Fall through to trapret to restore state and IRETQ.
     * Note: for user-mode traps, x86_trap_handler calls usertrapret()
     * and never returns here.  This path is only for kernel traps. */

.globl trapret
trapret:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Skip trapno and error code (2 * 8 = 16 bytes) */
    addq $16, %rsp

    /*
     * Return via IRETQ.
     *
     * No SWAPGS here.  User-mode returns never reach this point —
     * they go through usertrapret() → trampoline (which handles
     * SWAPGS + CR3 switch to user page table).
     * Kernel-mode traps did not SWAPGS on entry, so no SWAPGS on exit.
     */
    iretq

/* ──────────────────────────────────────────────────────────────
 *  vectors[] — table of 256 entry-point addresses for IDT setup.
 * ────────────────────────────────────────────────────────────── */
.macro EMIT_VEC_ADDR n
    .quad vector\n
.endm

.section .rodata,"a",@progbits
.align 8
.globl vectors
vectors:
.altmacro
.set i, 0
.rept 256
    EMIT_VEC_ADDR %i
    .set i, i + 1
.endr
.noaltmacro

/* ──────────────────────────────────────────────────────────────
 *  syscall_entry — SYSCALL instruction entry point (MSR_LSTAR).
 *
 *  On SYSCALL entry the CPU has:
 *    RCX = user RIP (return address)
 *    R11 = user RFLAGS
 *    CS/SS = kernel selectors from MSR_STAR
 *    Interrupts disabled (IF cleared by SFMASK)
 *    RSP = still user RSP (CPU does NOT switch stacks)
 *
 *  Strategy (SWAPGS for per-CPU scratch + fixed-address CR3):
 *    1. swapgs to get per-CPU scratch in GS base
 *       (SYSCALL always comes from user mode, so swapgs is unambiguous)
 *    2. Save user RSP to gs:0 (scratch)
 *    3. Load trapframe VA from gs:8
 *    4. Switch to kernel CR3 via trampoline_ksatp (RIP-relative)
 *    5. Save all user GPRs + metadata into utrapframe
 *    6. Load kernel stack from utrapframe.kernel_sp
 *    7. Call usertrap_syscall() in C
 *
 *  Utrapframe field offsets (struct trapframe then extra fields):
 *    0x00 r15,  0x08 r14,  0x10 r13,  0x18 r12
 *    0x20 r11,  0x28 r10,  0x30 r9,   0x38 r8
 *    0x40 rbp,  0x48 rdi,  0x50 rsi,  0x58 rdx
 *    0x60 rcx,  0x68 rbx,  0x70 rax
 *    0x78 trapno, 0x80 err, 0x88 rip, 0x90 cs
 *    0x98 rflags, 0xA0 rsp, 0xA8 ss
 *    ... _spare[11] ...
 *    0x120 kernel_sp
 * ────────────────────────────────────────────────────────────── */
.text
.align 16
.globl syscall_entry
syscall_entry:
    /*
     * Step 1: swap GS base to get per-CPU scratch area.
     * SYSCALL always comes from user-mode ring 3, so the swapgs
     * direction is unambiguous (unlike IDT traps).
     *
     * Before: GS_BASE = user gs (0), KERNEL_GS_BASE = &cpu_scratch[cpu]
     * After:  GS_BASE = &cpu_scratch[cpu], KERNEL_GS_BASE = user gs (0)
     */
    swapgs

    /*
     * Step 2: save user RSP, load trapframe VA.
     * gs:0 = scratch (for user RSP), gs:8 = trapframe VA
     */
    movq %rsp, %gs:0               /* save user RSP to scratch      */
    movq %gs:8, %rsp               /* rsp = utrapframe VA (temp)    */

    /*
     * Step 3: save all user registers into utrapframe.
     * rsp points to the utrapframe mapped in the USER page table.
     * We must finish all reads/writes to this user VA BEFORE
     * switching CR3 to the kernel page table.
     */
    movq %r15, 0x00(%rsp)
    movq %r14, 0x08(%rsp)
    movq %r13, 0x10(%rsp)
    movq %r12, 0x18(%rsp)
    movq %r11, 0x20(%rsp)          /* r11 = user RFLAGS             */
    movq %r10, 0x28(%rsp)
    movq %r9,  0x30(%rsp)
    movq %r8,  0x38(%rsp)
    movq %rbp, 0x40(%rsp)
    movq %rdi, 0x48(%rsp)
    movq %rsi, 0x50(%rsp)
    movq %rdx, 0x58(%rsp)
    movq %rcx, 0x60(%rsp)          /* rcx = user RIP                */
    movq %rbx, 0x68(%rsp)
    movq %rax, 0x70(%rsp)          /* rax = syscall number          */

    /* Metadata fields */
    movq $256, 0x78(%rsp)           /* trapno = 256 (syscall marker) */
    movq $0,   0x80(%rsp)           /* err = 0                       */
    movq %rcx, 0x88(%rsp)           /* rip = user RIP (from RCX)     */
    movq $(0x28 | 3), 0x90(%rsp)    /* cs  = SEG_UCODE | DPL_USER    */
    movq %r11, 0x98(%rsp)           /* rflags = user RFLAGS (from R11) */

    /* Retrieve user RSP from scratch and store in trapframe */
    movq %gs:0, %rax
    movq %rax, 0xA0(%rsp)           /* rsp = user RSP                */
    movq $(0x20 | 3), 0xA8(%rsp)    /* ss  = SEG_UDATA | DPL_USER    */

    /*
     * Step 4: read kernel_sp from trapframe, then switch CR3.
     * kernel_sp is at offset 0x120 — must read it while still
     * under user CR3 (the trapframe is at a user VA).
     */
    movq 0x120(%rsp), %r15          /* r15 = kernel_sp (user VA ok)  */

    leaq trampoline_ksatp(%rip), %rax
    movq (%rax), %rax
    movq %rax, %cr3                 /* switch to kernel page table   */

    /*
     * Step 5: switch to kernel stack and jump to C handler.
     * r15 was already saved to trapframe[0x00] above.
     */
    movq %r15, %rsp                 /* switch to kernel stack        */

    cld
    movabsq $usertrap_syscall, %rax
    call *%rax
    /* usertrap_syscall calls usertrapret() and never returns */
    ud2

/* ──────────────────────────────────────────────────────────────
 *  trampoline_ksatp — kernel page table (CR3) value.
 *
 *  Analogous to RISC-V trampoline_ksatp in trampoline.S.
 *  Set once at boot from arch_vm_init() in C code.
 *  Assembly accesses it RIP-relative from the high alias.
 *  C code accesses through the identity-mapped (direct) address.
 * ────────────────────────────────────────────────────────────── */
.align 8
.globl trampoline_ksatp
trampoline_ksatp:
    .quad 0

/*
 * trampoline_kstack — per-thread kernel stack top (p->ksp).
 *
 * Written by usertrapret() (C code, identity-mapped address) before
 * every return to user mode.  Read by alltraps (RIP-relative from
 * the high alias) on user→kernel transitions to pivot RSP from the
 * shared IST stack to the per-thread kernel stack.
 *
 * NOTE: single-CPU only.  For SMP, replace with a per-CPU slot
 *       (e.g. in struct cpu_local at TRAMPOLINE_CPULOCAL).
 */
.align 8
.globl trampoline_kstack
trampoline_kstack:
    .quad 0