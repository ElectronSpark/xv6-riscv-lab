#include "param.h"
        # qemu -kernel loads the kernel at 0x80200000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80200000.
.section .text.entry
.global _entry
_entry:
        # OpenSBI passes hartid in a0, save it to tp
        # (we're already in S-mode, so use tp to store hartid)
        mv tp, a0

        # Print early boot banner via SBI console (no memory access)
        # SBI legacy console putchar: a7 = 0x01, a0 = char
        li a7, 0x01

        # Print "\r\n[xv6] entry\r\n"
        li a0, '\r'
        ecall
        li a0, '\n'
        ecall
        li a0, '['
        ecall
        li a0, 'x'
        ecall
        li a0, 'v'
        ecall
        li a0, '6'
        ecall
        li a0, ']'
        ecall
        li a0, ' '
        ecall
        li a0, 'e'
        ecall
        li a0, 'n'
        ecall
        li a0, 't'
        ecall
        li a0, 'r'
        ecall
        li a0, 'y'
        ecall
        li a0, '\r'
        ecall
        li a0, '\n'
        ecall

        # Restore hartid to a0 (was clobbered by ecalls)
        mv a0, tp

        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)
        la sp, stack0
        li a2, KERNEL_STACK_SIZE
        addi a3, a0, 1
        mul a2, a2, a3
        add sp, sp, a2
        li a2, 4096    # Make room for PCB
        sub sp, sp, a2

        # jump to start() in start.c
        # a0 still contains hartid, a1 contains dtb pointer
        call start
spin:
        wfi
        j spin
