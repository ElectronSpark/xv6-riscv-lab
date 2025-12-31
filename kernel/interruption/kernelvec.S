
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
        addi a0, a0, -256
        sd sp, 8(a0)
        mv sp, a0
        csrrw a0, sscratch, a0
        sd a0, 72(sp)
        addi a0, sp, 256
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
        addi sp, sp, -256
        sd a0, 72(sp)
        addi a0, sp, 256
        sd a0, 8(sp)

__kernelvec:
        # save caller-saved registers.
        sd ra, 0(sp)
        sd s0, 16(sp)
        sd tp, 24(sp)
        sd t0, 32(sp)
        sd t1, 40(sp)
        sd t2, 48(sp)
        sd a1, 80(sp)
        sd a2, 88(sp)
        sd a3, 96(sp)
        sd a4, 104(sp)
        sd a5, 112(sp)
        sd a6, 120(sp)
        sd a7, 128(sp)
        sd t3, 216(sp)
        sd t4, 224(sp)
        sd t5, 232(sp)
        sd t6, 240(sp)
        sd gp, 248(sp)

        csrr t0, sepc
        sd t0, 56(sp)
        csrr t0, sstatus
        sd t0, 64(sp)
        csrr t0, scause
        sd t0, 136(sp)
        csrr t0, stval
        sd t0, 144(sp)
        csrr t0, stvec
        sd t0, 152(sp)

        # redirect all the following kernel trap to kernelvec
        la t0, kernelvec
        csrw stvec, t0

        # call the C trap handler in trap.c
        addi a0, sp, 0
        addi a1, s0, 0
        call kerneltrap

        ld t0, 56(sp)
        csrw sepc, t0
        ld t0, 64(sp)
        csrw sstatus, t0
        ld t0, 136(sp)
        csrw scause, t0
        ld t0, 144(sp)
        csrw stval, t0
        ld t0, 152(sp)
        csrw stvec, t0

        # restore registers.
        ld ra, 0(sp)
        ld s0, 16(sp)
        # not tp (contains hartid), in case we moved CPUs
        ld t0, 32(sp)
        ld t1, 40(sp)
        ld t2, 48(sp)
        ld a0, 72(sp)
        ld a1, 80(sp)
        ld a2, 88(sp)
        ld a3, 96(sp)
        ld a4, 104(sp)
        ld a5, 112(sp)
        ld a6, 120(sp)
        ld a7, 128(sp)
        ld t3, 216(sp)
        ld t4, 224(sp)
        ld t5, 232(sp)
        ld t6, 240(sp)
        ld gp, 248(sp)

        # restore sp at last
        ld sp, 8(sp)

        # return to whatever we were doing in the kernel.
        sret
