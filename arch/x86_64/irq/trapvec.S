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

#include "asm-offsets.h"

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
 *  Kernel mode: GS_BASE = per-CPU struct cpu_local.
 *  User mode:   GS_BASE = user GS, KERNEL_GS_BASE = per-CPU.
 *
 *  On IDT entry from user mode, we swapgs after checking CPL so
 *  the kernel gets per-CPU GS.  The user GS value (now in
 *  KERNEL_GS_BASE) is read by the C handler via rdmsr and stored
 *  in the utrapframe.  On return to user, usertrapret explicitly
 *  sets MSR_GS_BASE = user_gs_base and MSR_KERNEL_GS_BASE = mycpu()
 *  (Linux strategy: no swapgs chain, both MSRs set directly).
 *
 *  Kernel-mode traps skip swapgs (GS is already per-CPU).
 *
 *  CR3 is NOT switched here.  All user page tables share the
 *  kernel upper-half PML4 entries (256-511), so kernel text/data
 *  and the trampoline / per-CPU areas are accessible from any
 *  page table.  The C handler (x86_trap_handler / usertrap_syscall)
 *  switches CR3 to the kernel page table to gain identity-map
 *  (PML4[0]) access for PA-based pointers.
 *
 *  On IDT entry the CPU switches RSP to TSS.RSP0 (most vectors)
 *  or TSS.IST1/IST2 (NMI/#DF), so we are on a known kernel stack.
 * ────────────────────────────────────────────────────────────── */

.align 16
.globl alltraps
alltraps:
    /*
     * Stack layout at this point:
     *   [RSP+0x30]  SS
     *   [RSP+0x28]  RSP (user)
     *   [RSP+0x20]  RFLAGS
     *   [RSP+0x18]  CS
     *   [RSP+0x10]  RIP
     *   [RSP+0x08]  error code     (pushed by CPU or stub)
     *   [RSP+0x00]  vector number  (pushed by stub)
     *
     * Most vectors use IST=0: user→kernel transitions load RSP from
     * TSS.RSP0 (per-thread kstack); kernel→kernel traps stay on the
     * current stack.  Only NMI (IST=1) and #DF (IST=2) use IST.
     *
     * No CR3 switch here: all user page tables share PML4[256-511]
     * (kernel higher-half, trampoline, per-CPU areas).  The kstack
     * (PML4[256]) and kernel text/data are accessible from any page
     * table.  The C handler switches to the kernel CR3 when it needs
     * identity-map (PML4[0]) access.
     */

    /*
     * Copy interrupt frame to the per-thread kernel stack.
     *
     * For non-IST vectors from user mode, the CPU already loaded RSP
     * from TSS.RSP0 (= kstack top), so the copy below is a no-op
     * (source and destination are the same addresses).
     *
     * For IST vectors from user mode (NMI, #DF), the frame is on the
     * IST stack and must be copied to kstack for per-thread safety.
     *
     * Kernel-mode traps skip the copy and stay on the current stack.
     */
    testq $3, 24(%rsp)            /* CS & 3 → RPL                  */
    jz    .Lkstack_done           /* kernel mode → no switch needed */

    /*
     * User-mode entry: swap GS base so kernel sees per-CPU data.
     * Before: GS_BASE = user GS, KERNEL_GS_BASE = per-CPU
     * After:  GS_BASE = per-CPU,  KERNEL_GS_BASE = user GS
     * The user GS value (now in KERNEL_GS_BASE) is saved to the
     * trapframe later in the C handler via rdmsr.
     */
    swapgs

    /* ── FROM USER MODE ── copy interrupt frame to kstack ──
     *
     * Stack layout at this point (RSP-relative):
     *   +0  vec   +8  err   +16 rip   +24 cs
     *   +32 rflags   +40 user_rsp   +48 ss
     *
     * Save rax & rbx as scratch, then copy the 7 qwords into the
     * per-thread kstack whose top is in per-CPU intr_kstack_top
     * (set by usertrapret before every return to user mode).
     *
     * For non-IST vectors (most), RSP0 == intr_kstack_top so the
     * copy is a no-op (source == destination).  For IST vectors
     * (NMI, #DF), the frame is on the IST stack and actually moves.
     */
    pushq %rax                    /* save rax on stack             */
    pushq %rbx                    /* save rbx on stack             */
    /* Stack: rbx(+0) rax(+8) vec(+16) err(+24) rip(+32) cs(+40)
     *         rflags(+48) user_rsp(+56) ss(+64)                   */

    movq  %gs:CPU_LOCAL_INTR_KSTACK_TOP, %rax            /* rax = kstack top (= per-CPU intr_kstack_top) */

    /* Copy 7 values to kstack[-56 .. kstack-8].                   */
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
     * saved original rax on the stack, and finally load RSP from
     * that slot to complete the pivot.                             */
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
     * This path is only for kernel-mode traps.  User-mode returns
     * go through usertrapret() → userret (which does swapgs).
     * Kernel-mode traps did not swapgs on entry, so none on exit.
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
 *  Follows the Linux entry_64.S strategy:
 *    0. swapgs: GS_BASE = KERNEL_GS_BASE = mycpu() (set by usertrapret)
 *    1. Save user RSP to per-CPU scratch (%gs:CPU_LOCAL_SYSCALL_SCRATCH)
 *    2. Save registers directly into current->trapframe->trapframe
 *       (No CR3 switch — kernel is always visible via shared PML4[256-511])
 *    3. Load kernel stack from per-CPU (%gs:CPU_LOCAL_SYSCALL_KSTACK_TOP)
 *    4. Call usertrap_syscall(struct trapframe *tf)
 *
 *  GS state on entry: usertrapret always writes KERNEL_GS_BASE = mycpu()
 *  before returning to user mode (Linux strategy), so swapgs reliably
 *  yields GS_BASE = per-CPU regardless of prior swapgs chain state.
 *
 *  Linux x86-64 syscall calling convention:
 *    Syscall number : rax
 *    Arguments      : rdi, rsi, rdx, r10, r8, r9  (up to 6)
 *    Return value   : rax
 *    Preserved      : rdi, rsi, rdx, r10, r8, r9, rbx, rbp, r12–r15
 *    Clobbered      : rcx, r11  ("transaction registers")
 *
 *  The SYSCALL instruction itself sets:
 *    RCX = user RIP (return address)
 *    R11 = user RFLAGS
 *    CS/SS = kernel selectors from MSR_STAR
 *    Interrupts disabled (IF cleared by SFMASK)
 *    RSP = still user RSP (CPU does NOT switch stacks)
 *
 *  Per-CPU offsets (GS_BASE = struct cpu_local after swapgs):
 *    %gs:CPU_LOCAL_SYSCALL_SCRATCH     syscall_scratch    (stash user RSP)
 *    %gs:CPU_LOCAL_SYSCALL_KSTACK_TOP  syscall_kstack_top (kernel stack top)
 * ────────────────────────────────────────────────────────────── */
.text
.align 16
.globl syscall_entry
syscall_entry:
    /*
     * Step 0: swap GS base.  SYSCALL always comes from user mode.
     * Before: GS_BASE = user GS, KERNEL_GS_BASE = per-CPU
     * After:  GS_BASE = per-CPU,  KERNEL_GS_BASE = user GS
     */
    swapgs

    /*
     * Step 1: save user RSP to per-CPU scratch.
     * After this, RSP is free to use as a scratch register.
     */
    movq %rsp, %gs:CPU_LOCAL_SYSCALL_SCRATCH

    /*
     * Step 2: store the register frame directly in the thread's utrapframe.
     * usertrapret caches a higher-half trapframe VA in cpu_local so this
     * code never dereferences low kernel-stack addresses under the user CR3.
     *
     * rcx = user RIP, r11 = user RFLAGS (set by SYSCALL HW).  Save user r15
     * before using it as scratch for the saved user RSP.
     */
    movq %gs:CPU_LOCAL_SYSCALL_TRAPFRAME, %rsp

    movq %r15, TRAPFRAME_R15(%rsp)
    movq %r14, TRAPFRAME_R14(%rsp)
    movq %r13, TRAPFRAME_R13(%rsp)
    movq %r12, TRAPFRAME_R12(%rsp)
    movq %r11, TRAPFRAME_R11(%rsp)     /* clobbered: user RFLAGS */
    movq %r10, TRAPFRAME_R10(%rsp)
    movq %r9,  TRAPFRAME_R9(%rsp)
    movq %r8,  TRAPFRAME_R8(%rsp)
    movq %rbp, TRAPFRAME_RBP(%rsp)
    movq %rdi, TRAPFRAME_RDI(%rsp)
    movq %rsi, TRAPFRAME_RSI(%rsp)
    movq %rdx, TRAPFRAME_RDX(%rsp)
    movq %rcx, TRAPFRAME_RCX(%rsp)     /* clobbered: user RIP */
    movq %rbx, TRAPFRAME_RBX(%rsp)
    movq %rax, TRAPFRAME_RAX(%rsp)     /* syscall number */

    movq $256,       TRAPFRAME_TRAPNO(%rsp)
    movq $0,         TRAPFRAME_ERR(%rsp)
    movq %rcx,       TRAPFRAME_RIP(%rsp)
    movq $(0x28 | 3), TRAPFRAME_CS(%rsp)
    movq %r11,       TRAPFRAME_RFLAGS(%rsp)
    movq %gs:CPU_LOCAL_SYSCALL_SCRATCH, %r15
    movq %r15,       TRAPFRAME_RSP(%rsp)
    movq $(0x20 | 3), TRAPFRAME_SS(%rsp)

    /*
     * Step 3: move to the kernel stack before calling C.
     * RDI = pointer to the authoritative thread trapframe.
     */
    movq %rsp, %rdi
    movq %gs:CPU_LOCAL_SYSCALL_KSTACK_TOP, %rsp

    /*
     * Step 4: call C handler.
     */
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
 * trampoline_kstack — legacy per-thread kernel stack top slot.
 *
 * Previously used by alltraps on single-CPU builds.  Now replaced by
 * per-CPU intr_kstack_top in struct cpu_local (%gs:CPU_LOCAL_INTR_KSTACK_TOP).
 * Kept for ABI compatibility (vm.c references it for trampoline sizing).
 */
.align 8
.globl trampoline_kstack
trampoline_kstack:
    .quad 0

/* End-of-trapvec marker — used by arch_vm_init() to know how many
 * pages to map into the high-canonical alias window. */
.globl trapvec_end
trapvec_end:

/*
 * syscall_entry uses GS-based per-CPU fields (Linux strategy):
 *   %gs:CPU_LOCAL_SYSCALL_SCRATCH  cpu_local.syscall_scratch     — stash user RSP
 *   %gs:CPU_LOCAL_SYSCALL_KSTACK_TOP  cpu_local.syscall_kstack_top  — kernel stack top
 * Written by usertrapret() via mycpu()->syscall_kstack_top.
 */
