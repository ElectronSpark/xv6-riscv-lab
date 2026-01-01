#include "asm-offsets.h"
        #
        # low-level code to handle traps from user space into
        # the kernel, and returns from kernel to user.
        #
        # the kernel maps the page holding this code
        # at the same virtual address (TRAMPOLINE)
        # in user and kernel space so that it continues
        # to work when it switches page tables.
        # kernel.ld causes this code to start at 
        # a page boundary.
        #

#include "riscv.h"
#include "memlayout.h"

.section trampsec
.globl trampoline
.globl usertrap
trampoline:
.align 4
.globl uservec
uservec:    
	#
        # trap.c sets stvec to point here, so
        # traps from user space start here,
        # in supervisor mode, but with a
        # user page table.
        #

        # save user a0 in sscratch so
        # a0 can be used to get at TRAPFRAME.
        # sscratch also stores the trapframe base address
        csrrw a0, sscratch, a0

        # save the user registers in TRAPFRAME
        sd ra, TRAPFRAME_RA(a0)
        sd sp, TRAPFRAME_SP(a0)
        sd t0, TRAPFRAME_T0(a0)
        sd t1, TRAPFRAME_T1(a0)
        sd t2, TRAPFRAME_T2(a0)
        sd s0, TRAPFRAME_S0(a0)
        sd s1, TRAPFRAME_S1(a0)
        sd a1, TRAPFRAME_A1(a0)
        sd a2, TRAPFRAME_A2(a0)
        sd a3, TRAPFRAME_A3(a0)
        sd a4, TRAPFRAME_A4(a0)
        sd a5, TRAPFRAME_A5(a0)
        sd a6, TRAPFRAME_A6(a0)
        sd a7, TRAPFRAME_A7(a0)
        sd s2, TRAPFRAME_S2(a0)
        sd s3, TRAPFRAME_S3(a0)
        sd s4, TRAPFRAME_S4(a0)
        sd s5, TRAPFRAME_S5(a0)
        sd s6, TRAPFRAME_S6(a0)
        sd s7, TRAPFRAME_S7(a0)
        sd s8, TRAPFRAME_S8(a0)
        sd s9, TRAPFRAME_S9(a0)
        sd s10, TRAPFRAME_S10(a0)
        sd s11, TRAPFRAME_S11(a0)
        sd t3, TRAPFRAME_T3(a0)
        sd t4, TRAPFRAME_T4(a0)
        sd t5, TRAPFRAME_T5(a0)
        sd t6, TRAPFRAME_T6(a0)
        sd gp, TRAPFRAME_GP(a0)
        sd tp, TRAPFRAME_TP(a0)
	# save the user a0 in p->trapframe->a0
        csrrw t0, sscratch, t0
        sd t0, TRAPFRAME_A0(a0)

        # save scause, sstatus, stval, and sepc into the trapframe
        csrr t0, stval
        sd t0, TRAPFRAME_STVAL(a0)
        csrr t0, sepc
        sd t0, TRAPFRAME_SEPC(a0)
        csrr t0, sstatus
        sd t0, TRAPFRAME_SSTATUS(a0)
        csrr t0, scause
        sd t0, TRAPFRAME_SCAUSE(a0)

        srli t0, t0, 63         # is interrupt
        beqz t0, 2f             # if not, jump to handle exception
1:
        ld sp, TRAPFRAME_IRQ_SP(a0)   # load irq_sp from trapframe
        ld t2, TRAPFRAME_IRQ_ENTRY(a0)   # load irq_entry from trapframe
        j 3f
2:
        # initialize kernel stack pointer, from p->trapframe->kernel_sp
        ld sp, TRAPFRAME_KERNEL_SP(a0)
        ld t2, TRAPFRAME_KERNEL_TRAP(a0)   # load kernel_trap from trapframe
3:
        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld tp, TRAPFRAME_KERNEL_HARTID(a0)
        #make sure gp is also restored from trapframe
        ld gp, TRAPFRAME_KERNEL_GP(a0)

        # load the address of usertrap(), from p->trapframe->kernel_trap
        ld t0, TRAPFRAME_KERNEL_TRAP(a0)

        # fetch the kernel page table address, from p->trapframe->kernel_satp.
        ld t1, TRAPFRAME_KERNEL_SATP(a0)

        # wait for any previous memory operations to complete, so that
        # they use the user page table.
        sfence.vma zero, zero

        # install the kernel page table.
        csrw satp, t1

        # flush now-stale user entries from the TLB.
        sfence.vma zero, zero

        # jump to usertrap(), which does not return
        jr t2

.globl userret
userret:
        # userret(pagetable)
        # called by usertrapret() in trap.c to
        # switch from kernel to user.
        # a0: trapframe base
        # a1: user page table, for satp.

        # switch to the user page table.
        sfence.vma zero, zero
        csrw satp, a1
        sfence.vma zero, zero

        # store trapframe base in sscratch
        csrw sscratch, a0

        # @TODO: restore sstatus from trapframe
        # restore sepc from the trapframe
        ld t0, TRAPFRAME_SEPC(a0)
        csrw sepc, t0

        # restore all but a0 from TRAPFRAME
        ld ra, TRAPFRAME_RA(a0)
        ld sp, TRAPFRAME_SP(a0)
        ld t0, TRAPFRAME_T0(a0)
        ld t1, TRAPFRAME_T1(a0)
        ld t2, TRAPFRAME_T2(a0)
        ld s0, TRAPFRAME_S0(a0)
        ld s1, TRAPFRAME_S1(a0)
        ld a1, TRAPFRAME_A1(a0)
        ld a2, TRAPFRAME_A2(a0)
        ld a3, TRAPFRAME_A3(a0)
        ld a4, TRAPFRAME_A4(a0)
        ld a5, TRAPFRAME_A5(a0)
        ld a6, TRAPFRAME_A6(a0)
        ld a7, TRAPFRAME_A7(a0)
        ld s2, TRAPFRAME_S2(a0)
        ld s3, TRAPFRAME_S3(a0)
        ld s4, TRAPFRAME_S4(a0)
        ld s5, TRAPFRAME_S5(a0)
        ld s6, TRAPFRAME_S6(a0)
        ld s7, TRAPFRAME_S7(a0)
        ld s8, TRAPFRAME_S8(a0)
        ld s9, TRAPFRAME_S9(a0)
        ld s10, TRAPFRAME_S10(a0)
        ld s11, TRAPFRAME_S11(a0)
        ld t3, TRAPFRAME_T3(a0)
        ld t4, TRAPFRAME_T4(a0)
        ld t5, TRAPFRAME_T5(a0)
        ld t6, TRAPFRAME_T6(a0)
        ld gp, TRAPFRAME_KERNEL_GP(a0)
        ld tp, TRAPFRAME_KERNEL_HARTID(a0)

	# restore user a0
        ld a0, TRAPFRAME_A0(a0)
        
        # return to user mode and user pc.
        # usertrapret() set up sstatus and sepc.
        sret
