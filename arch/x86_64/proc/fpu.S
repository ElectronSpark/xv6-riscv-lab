/*
 * fpu.S — x86_64 floating-point save / restore / init helpers.
 *
 * These routines are called from the kernel's lazy-FPU-switch path.
 * The kernel is compiled with -mgeneral-regs-only, but these hand-
 * written assembly functions are allowed to use FP/SSE instructions
 * because they explicitly manage the state.
 *
 * Calling convention (System V AMD64):
 *   %rdi = pointer to struct fpu_state  { uint8 fxsave_area[512]; }
 *          The area MUST be 16-byte aligned.
 */

.section .text

/*
 * void fpu_save_state(struct fpu_state *state)
 *
 * Save x87 + SSE state via FXSAVE.
 */
.globl fpu_save_state
.type  fpu_save_state, @function
fpu_save_state:
    fxsave (%rdi)
    ret

/*
 * void fpu_restore_state(struct fpu_state *state)
 *
 * Restore x87 + SSE state via FXRSTOR.
 */
.globl fpu_restore_state
.type  fpu_restore_state, @function
fpu_restore_state:
    fxrstor (%rdi)
    ret

/*
 * void fpu_init_state(void)
 *
 * Reset x87 + SSE to a clean initial state.
 *   - FNINIT resets x87 (FCW=0x037F, FSW=0, FTW=0xFFFF).
 *   - LDMXCSR sets SSE MXCSR to default (0x1F80: all exceptions masked).
 *   - XMM0-XMM15 are zeroed via XORPS pairs.
 */
.globl fpu_init_state
.type  fpu_init_state, @function
fpu_init_state:
    fninit
    /* Set MXCSR to default value (0x1F80) */
    subq  $8, %rsp
    movl  $0x1F80, (%rsp)
    ldmxcsr (%rsp)
    addq  $8, %rsp
    /* Zero all XMM registers */
    xorps  %xmm0,  %xmm0
    xorps  %xmm1,  %xmm1
    xorps  %xmm2,  %xmm2
    xorps  %xmm3,  %xmm3
    xorps  %xmm4,  %xmm4
    xorps  %xmm5,  %xmm5
    xorps  %xmm6,  %xmm6
    xorps  %xmm7,  %xmm7
    xorps  %xmm8,  %xmm8
    xorps  %xmm9,  %xmm9
    xorps  %xmm10, %xmm10
    xorps  %xmm11, %xmm11
    xorps  %xmm12, %xmm12
    xorps  %xmm13, %xmm13
    xorps  %xmm14, %xmm14
    xorps  %xmm15, %xmm15
    ret
