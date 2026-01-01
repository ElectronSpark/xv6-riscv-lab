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
        addi a0, a0, -KTRAPFRAME_SIZE
        sd sp, KTRAPFRAME_SP(a0)
        mv sp, a0
        csrrw a0, sscratch, a0
        sd a0, KTRAPFRAME_A0(sp)
        addi a0, sp, KTRAPFRAME_SIZE
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
        addi sp, sp, -KTRAPFRAME_SIZE
        sd a0, KTRAPFRAME_A0(sp)
        addi a0, sp, KTRAPFRAME_SIZE
        sd a0, KTRAPFRAME_SP(sp)

__kernelvec:
        # save caller-saved registers.
        sd ra, KTRAPFRAME_RA(sp)
        sd s0, KTRAPFRAME_S0(sp)
        sd t0, KTRAPFRAME_T0(sp)
        sd t1, KTRAPFRAME_T1(sp)
        sd t2, KTRAPFRAME_T2(sp)
        sd a1, KTRAPFRAME_A1(sp)
        sd a2, KTRAPFRAME_A2(sp)
        sd a3, KTRAPFRAME_A3(sp)
        sd a4, KTRAPFRAME_A4(sp)
        sd a5, KTRAPFRAME_A5(sp)
        sd a6, KTRAPFRAME_A6(sp)
        sd a7, KTRAPFRAME_A7(sp)
        sd t3, KTRAPFRAME_T3(sp)
        sd t4, KTRAPFRAME_T4(sp)
        sd t5, KTRAPFRAME_T5(sp)
        sd t6, KTRAPFRAME_T6(sp)

        csrr t0, sepc
        sd t0, KTRAPFRAME_SEPC(sp)
        csrr t0, sstatus
        sd t0, KTRAPFRAME_SSTATUS(sp)
        csrr t0, scause
        sd t0, KTRAPFRAME_SCAUSE(sp)
        csrr t0, stval
        sd t0, KTRAPFRAME_STVAL(sp)
        csrr t0, stvec
        sd t0, KTRAPFRAME_STVEC(sp)

        # redirect all the following kernel trap to kernelvec
        la t0, kernelvec
        csrw stvec, t0

        # call the C trap handler in trap.c
        addi a0, sp, 0
        addi a1, s0, 0
        call kerneltrap

        ld t0, KTRAPFRAME_SEPC(sp)
        csrw sepc, t0
        ld t0, KTRAPFRAME_SSTATUS(sp)
        csrw sstatus, t0
        ld t0, KTRAPFRAME_SCAUSE(sp)
        csrw scause, t0
        ld t0, KTRAPFRAME_STVAL(sp)
        csrw stval, t0
        ld t0, KTRAPFRAME_STVEC(sp)
        csrw stvec, t0

        # restore registers.
        ld ra, KTRAPFRAME_RA(sp)
        ld s0, KTRAPFRAME_S0(sp)
        # not tp (contains hartid) and gp, in case we moved CPUs
        ld t0, KTRAPFRAME_T0(sp)
        ld t1, KTRAPFRAME_T1(sp)
        ld t2, KTRAPFRAME_T2(sp)
        ld a0, KTRAPFRAME_A0(sp)
        ld a1, KTRAPFRAME_A1(sp)
        ld a2, KTRAPFRAME_A2(sp)
        ld a3, KTRAPFRAME_A3(sp)
        ld a4, KTRAPFRAME_A4(sp)
        ld a5, KTRAPFRAME_A5(sp)
        ld a6, KTRAPFRAME_A6(sp)
        ld a7, KTRAPFRAME_A7(sp)
        ld t3, KTRAPFRAME_T3(sp)
        ld t4, KTRAPFRAME_T4(sp)
        ld t5, KTRAPFRAME_T5(sp)
        ld t6, KTRAPFRAME_T6(sp)

        # restore sp at last
        ld sp, KTRAPFRAME_SP(sp)

        # return to whatever we were doing in the kernel.
        sret
