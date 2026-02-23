# Initial process that execs /bin/init.
# This code runs in user space on x86_64.
# Uses SYSCALL instruction with Linux x86_64 convention:
#   rax = syscall number, rdi = arg0, rsi = arg1

#include "kernel/inc/syscall.h"

.globl start
start:
        leaq init(%rip), %rdi         # arg0 = path
        leaq argv(%rip), %rsi         # arg1 = argv
        movq $SYS_exec, %rax
        syscall

        # for(;;) exit();
exit_loop:
        movq $SYS_exit, %rax
        syscall
        jmp exit_loop

# char init[] = "/bin/init\0";
init:
        .string "/bin/init"

# char *argv[] = { init, 0 };
.p2align 3
argv:
        .quad init
        .quad 0
