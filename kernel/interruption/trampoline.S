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

.globl kernelvec
.globl usertrap
.globl user_kirq_entrance

.section trampdatasec
.globl trampoline_kvec
.align 8
trampoline_kvec:
    .dword kernelvec      # kernelvec is in kernel/trap.c
.globl trampoline_utrap
.align 8
trampoline_utrap:
    .dword usertrap        # usertrap is defined below
.globl trampoline_ksatp
.align 8
trampoline_ksatp:
    .dword 0              # filled in by trap.c: usertrapret()
.globl trampoline_kirq
.align 8
trampoline_kirq:
    .dword user_kirq_entrance  # user_kirq_entrance is in kernel/trap.c
.globl trampoline_trapframe_base
.align 8
trampoline_trapframe_base:
    .dword 0              # filled in by trap.c: usertrapret()
.globl trampoline_uservec
.align 8
trampoline_uservec:
    .dword 0              # filled in by trap.c: trapinit()

.section trampsec
.globl trampoline
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

        # set kernelvec as the trap handler
        ld a0, trampoline_kvec
        csrw stvec, a0

        ld a0, trampoline_trapframe_base

        # save the user registers in TRAPFRAME
        sd ra, TRAPFRAME_RA(a0)
        sd sp, TRAPFRAME_SP(a0)
        sd t0, TRAPFRAME_T0(a0)
        sd t1, TRAPFRAME_T1(a0)
        sd t2, TRAPFRAME_T2(a0)
        sd t3, TRAPFRAME_T3(a0)
        sd t4, TRAPFRAME_T4(a0)
        sd t5, TRAPFRAME_T5(a0)
        sd t6, TRAPFRAME_T6(a0)
        sd a1, TRAPFRAME_A1(a0)
        sd a2, TRAPFRAME_A2(a0)
        sd a3, TRAPFRAME_A3(a0)
        sd a4, TRAPFRAME_A4(a0)
        sd a5, TRAPFRAME_A5(a0)
        sd a6, TRAPFRAME_A6(a0)
        sd a7, TRAPFRAME_A7(a0)
        sd s0, TRAPFRAME_S0(a0)
        sd s1, UTRAPFRAME_S1(a0)
        sd s2, UTRAPFRAME_S2(a0)
        sd s3, UTRAPFRAME_S3(a0)
        sd s4, UTRAPFRAME_S4(a0)
        sd s5, UTRAPFRAME_S5(a0)
        sd s6, UTRAPFRAME_S6(a0)
        sd s7, UTRAPFRAME_S7(a0)
        sd s8, UTRAPFRAME_S8(a0)
        sd s9, UTRAPFRAME_S9(a0)
        sd s10, UTRAPFRAME_S10(a0)
        sd s11, UTRAPFRAME_S11(a0)
        sd gp, UTRAPFRAME_GP(a0)
        sd tp, UTRAPFRAME_TP(a0)
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
        ld sp, UTRAPFRAME_IRQ_SP(a0)   # load irq_sp from trapframe
        ld t2, trampoline_kirq   # load irq_entry from trapframe
        j 3f
2:
        # initialize kernel stack pointer, from p->trapframe->kernel_sp
        ld sp, UTRAPFRAME_KERNEL_SP(a0)
        ld t2, trampoline_utrap
3:
        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld tp, UTRAPFRAME_KERNEL_HARTID(a0)
        #make sure gp is also restored from trapframe
        ld gp, UTRAPFRAME_KERNEL_GP(a0)

        # load the address of usertrap(), from p->trapframe->kernel_trap
        ld t0, UTRAPFRAME_KERNEL_TRAP(a0)

        # wait for any previous memory operations to complete, so that
        # they use the user page table.
        sfence.vma zero, zero

        # install the kernel page table.
        ld t1, trampoline_ksatp
        csrw satp, t1

        # flush now-stale user entries from the TLB.
        sfence.vma zero, zero

        # jump to usertrap (exceptions) or user_kirq_entrance (interrupts)
        # t2 was loaded in path 1 or 2 above
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
        csrw satp, a0
        sfence.vma zero, zero

        ld a0, trampoline_uservec
        csrw stvec, a0

        # store trapframe base in sscratch
        ld a0, trampoline_trapframe_base

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
        ld t3, TRAPFRAME_T3(a0)
        ld t4, TRAPFRAME_T4(a0)
        ld t5, TRAPFRAME_T5(a0)
        ld t6, TRAPFRAME_T6(a0)
        ld a1, TRAPFRAME_A1(a0)
        ld a2, TRAPFRAME_A2(a0)
        ld a3, TRAPFRAME_A3(a0)
        ld a4, TRAPFRAME_A4(a0)
        ld a5, TRAPFRAME_A5(a0)
        ld a6, TRAPFRAME_A6(a0)
        ld a7, TRAPFRAME_A7(a0)
        ld s0, TRAPFRAME_S0(a0)
        ld s1, UTRAPFRAME_S1(a0)
        ld s2, UTRAPFRAME_S2(a0)
        ld s3, UTRAPFRAME_S3(a0)
        ld s4, UTRAPFRAME_S4(a0)
        ld s5, UTRAPFRAME_S5(a0)
        ld s6, UTRAPFRAME_S6(a0)
        ld s7, UTRAPFRAME_S7(a0)
        ld s8, UTRAPFRAME_S8(a0)
        ld s9, UTRAPFRAME_S9(a0)
        ld s10, UTRAPFRAME_S10(a0)
        ld s11, UTRAPFRAME_S11(a0)
        sd gp, UTRAPFRAME_KERNEL_GP(a0)
        sd tp, UTRAPFRAME_KERNEL_HARTID(a0)
        ld gp, UTRAPFRAME_GP(a0)
        ld tp, UTRAPFRAME_TP(a0)

	# restore user a0
        ld a0, TRAPFRAME_A0(a0)
        
        # return to user mode and user pc.
        # usertrapret() set up sstatus and sepc.
        sret
