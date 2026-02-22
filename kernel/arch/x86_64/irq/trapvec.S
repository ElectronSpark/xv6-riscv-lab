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
 * ────────────────────────────────────────────────────────────── */
.align 16
.globl alltraps
alltraps:
    /* At this point the stack looks like:
     *   [RSP+0x10]  RIP / CS / RFLAGS / RSP / SS  (pushed by CPU)
     *   [RSP+0x08]  error code                     (pushed by CPU or stub)
     *   [RSP+0x00]  vector number                  (pushed by stub)
     *
     * We now push all 15 GPRs to build a full struct trapframe.
     */
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

    /* Call the C handler: void x86_trap_handler(struct trapframe *tf) */
    call x86_trap_handler

    /* Fall through to trapret to restore state and IRETQ */

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