#include "param.h"
        # qemu -kernel loads the kernel at 0x80000000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80000000.
.section .text.entry
.global _entry
_entry:
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a KERNEL_STACK_SIZE-byte stack per CPU.
        # sp = stack0 + (hartid * KERNEL_STACK_SIZE)
        la sp, stack0
        li a0, KERNEL_STACK_SIZE
        csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
        # jump to start() in start.c
        call start
spin:
        j spin
