#include "asm-offsets.h"

        #
        # interrupts and exceptions while in supervisor
        # mode come here.
        #
        # the current stack is a kernel stack, and the interrupt 
        # stack is stored in sscratch
        # push registers, call kerneltrap().
        # when kerneltrap() returns, restore registers, return.
        #
.globl kerneltrap
.globl kernelvec_entrance
.align 4
kernelvec_entrance:
        csrrw a0, sscratch, a0
        # make room to save registers.
        addi a0, a0, -TRAPFRAME_SIZE
        sd sp, TRAPFRAME_SP(a0)
        mv sp, a0
        csrrw a0, sscratch, a0
        sd a0, TRAPFRAME_A0(sp)
        addi a0, sp, TRAPFRAME_SIZE
        csrrw a0, sscratch, a0
        j __kernelvec

        # the current stack is a interrupt stack.
        # push registers, call kerneltrap().
        # when kerneltrap() returns, restore registers, return.
        # This will be a recursive interrupt
        #
.globl kernelvec
.align 4
kernelvec:
        # make room to save registers.
        addi sp, sp, -TRAPFRAME_SIZE
        sd a0, TRAPFRAME_A0(sp)
        addi a0, sp, TRAPFRAME_SIZE
        sd a0, TRAPFRAME_SP(sp)

__kernelvec:
        # save caller-saved registers.
        sd ra, TRAPFRAME_RA(sp)
        sd s0, TRAPFRAME_S0(sp)
        sd t0, TRAPFRAME_T0(sp)
        sd t1, TRAPFRAME_T1(sp)
        sd t2, TRAPFRAME_T2(sp)
        sd a1, TRAPFRAME_A1(sp)
        sd a2, TRAPFRAME_A2(sp)
        sd a3, TRAPFRAME_A3(sp)
        sd a4, TRAPFRAME_A4(sp)
        sd a5, TRAPFRAME_A5(sp)
        sd a6, TRAPFRAME_A6(sp)
        sd a7, TRAPFRAME_A7(sp)
        sd t3, TRAPFRAME_T3(sp)
        sd t4, TRAPFRAME_T4(sp)
        sd t5, TRAPFRAME_T5(sp)
        sd t6, TRAPFRAME_T6(sp)

        csrr t0, sepc
        sd t0, TRAPFRAME_SEPC(sp)
        csrr t0, sstatus
        sd t0, TRAPFRAME_SSTATUS(sp)
        csrr t0, scause
        sd t0, TRAPFRAME_SCAUSE(sp)
        csrr t0, stval
        sd t0, TRAPFRAME_STVAL(sp)
        csrr t0, stvec
        sd t0, TRAPFRAME_STVEC(sp)

        # redirect all the following kernel trap to kernelvec
        la t0, kernelvec
        csrw stvec, t0

        # call the C trap handler in trap.c
        addi a0, sp, 0
        addi a1, s0, 0
        call kerneltrap

        ld t0, TRAPFRAME_SEPC(sp)
        csrw sepc, t0
        ld t0, TRAPFRAME_SSTATUS(sp)
        csrw sstatus, t0
        ld t0, TRAPFRAME_SCAUSE(sp)
        csrw scause, t0
        ld t0, TRAPFRAME_STVAL(sp)
        csrw stval, t0
        ld t0, TRAPFRAME_STVEC(sp)
        csrw stvec, t0

        # restore registers.
        ld ra, TRAPFRAME_RA(sp)
        ld s0, TRAPFRAME_S0(sp)
        # not tp (contains hartid) and gp, in case we moved CPUs
        ld t0, TRAPFRAME_T0(sp)
        ld t1, TRAPFRAME_T1(sp)
        ld t2, TRAPFRAME_T2(sp)
        ld a0, TRAPFRAME_A0(sp)
        ld a1, TRAPFRAME_A1(sp)
        ld a2, TRAPFRAME_A2(sp)
        ld a3, TRAPFRAME_A3(sp)
        ld a4, TRAPFRAME_A4(sp)
        ld a5, TRAPFRAME_A5(sp)
        ld a6, TRAPFRAME_A6(sp)
        ld a7, TRAPFRAME_A7(sp)
        ld t3, TRAPFRAME_T3(sp)
        ld t4, TRAPFRAME_T4(sp)
        ld t5, TRAPFRAME_T5(sp)
        ld t6, TRAPFRAME_T6(sp)

        # restore sp at last
        ld sp, TRAPFRAME_SP(sp)

        # return to whatever we were doing in the kernel.
        sret
