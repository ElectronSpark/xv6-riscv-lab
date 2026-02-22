#ifndef __KERNEL_X86_64_TRAPFRAME_H
#define __KERNEL_X86_64_TRAPFRAME_H

#include "types.h"

/**
 * @brief x86_64 kernel-mode trap frame.
 *
 * Layout must match what trapvec.S pushes.  The CPU pushes SS, RSP,
 * RFLAGS, CS, RIP (and sometimes an error code); our stub normalises
 * the error code, pushes the vector number, then all GPRs.
 *
 * Stack grows downward, so struct fields at **lower** offsets are at
 * **lower** addresses (pushed last).
 */
struct trapframe {
    /* ── pushed by our assembly stub (low → high address) ── */
    uint64 r15;       /*  0x00 */
    uint64 r14;       /*  0x08 */
    uint64 r13;       /*  0x10 */
    uint64 r12;       /*  0x18 */
    uint64 r11;       /*  0x20 */
    uint64 r10;       /*  0x28 */
    uint64 r9;        /*  0x30 */
    uint64 r8;        /*  0x38 */
    uint64 rbp;       /*  0x40 */
    uint64 rdi;       /*  0x48 */
    uint64 rsi;       /*  0x50 */
    uint64 rdx;       /*  0x58 */
    uint64 rcx;       /*  0x60 */
    uint64 rbx;       /*  0x68 */
    uint64 rax;       /*  0x70 */

    /*
     * Anonymous unions provide backward-compat RISC-V names
     * (sepc, scause, stval, sstatus, sp, ra) alongside the
     * the canonical x86 names, without polluting the macro namespace.
     *
     * tf->ra and tf->sepc both alias tf->rip, etc.
     */
    union { uint64 trapno; uint64 scause; };  /*  0x78  ── vector number ── */

    /* ── pushed by CPU (or dummy 0 by stub for vectors w/o err) ── */
    union { uint64 err; uint64 stval; };      /*  0x80 */

    /* ── pushed by CPU on trap entry ── */
    union { uint64 rip; uint64 sepc; uint64 ra; }; /*  0x88 */
    uint64 cs;        /*  0x90 */
    union { uint64 rflags; uint64 sstatus; }; /*  0x98 */
    union { uint64 rsp; uint64 sp; };         /*  0xA0  (caller / user RSP) */
    uint64 ss;        /*  0xA8 */
} __ALIGNED(8);

/* ──────────────────────────────────────────────────────────────
 *  User trap frame – extends kernel trapframe with the state
 *  needed for user↔kernel transitions.
 *  TODO: flesh out when implementing user-mode.
 * ────────────────────────────────────────────────────────────── */
struct utrapframe {
    struct trapframe trapframe;

    uint64 _spare[11]; /* placeholder: s1-s11 equivalent space */

    uint64 irq_sp;
    uint64 irq_entry;
    uint64 kernel_satp;
    uint64 kernel_sp;
    uint64 kernel_trap;
    uint64 tp;
    uint64 kernel_hartid;
    uint64 gp;
    uint64 kernel_gp;
} __ALIGNED(8);

/* ──────────────────────────────────────────────────────────────
 *  Context-switch save area  (used by arch_context_switch / swtch).
 *  Only callee-saved registers (System V AMD64 ABI) plus the
 *  return address and stack pointer.
 * ────────────────────────────────────────────────────────────── */
struct context {
    uint64 ra;   /* return address (pushed by CALL) */
    uint64 rsp;

    /* callee-saved */
    uint64 rbx;
    uint64 rbp;
    uint64 r12;
    uint64 r13;
    uint64 r14;
    uint64 r15;
} __ALIGNED_CACHELINE;

#endif /* __KERNEL_X86_64_TRAPFRAME_H */