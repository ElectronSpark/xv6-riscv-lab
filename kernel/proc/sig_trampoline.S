#include "riscv.h"
#include "memlayout.h"
#include "syscall.h"

.section sig_trampsec
.global sig_trampoline
sig_trampoline:
# call the signal handler
    jalr ra, t0, 0

    li a7, SYS_sigreturn
    ecall
# should never return to this place
    addi a0, x0, 0
    sd a0, 0(a0)