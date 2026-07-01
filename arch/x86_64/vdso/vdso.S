#define CLOCK_MONOTONIC        1
#define CLOCK_MONOTONIC_RAW    4
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME         7
#define CLOCK_BOOTTIME_ALARM   9
#define SYS_clock_gettime      228
#define SYS_clock_getres       229

    .section .rodata
    .balign 8
    .globl xv6_vdso_data_marker
    .hidden xv6_vdso_data_marker
xv6_vdso_data_marker:
    .quad 0x317664736f367678
    .quad 0xce899b8c90c98987
xv6_vdso_timebase_freq:
    .quad 0

    .text
    .balign 16
    .globl __vdso_clock_gettime
    .type __vdso_clock_gettime, @function
__vdso_clock_gettime:
    cmpl $CLOCK_MONOTONIC, %edi
    je 1f
    cmpl $CLOCK_MONOTONIC_RAW, %edi
    je 1f
    cmpl $CLOCK_MONOTONIC_COARSE, %edi
    je 1f
    cmpl $CLOCK_BOOTTIME, %edi
    je 1f
    cmpl $CLOCK_BOOTTIME_ALARM, %edi
    je 1f
    movq $SYS_clock_gettime, %rax
    syscall
    ret
1:
    testq %rsi, %rsi
    je 3f
    movq xv6_vdso_timebase_freq(%rip), %rcx
    testq %rcx, %rcx
    je 3f
    rdtsc
    shlq $32, %rdx
    orq %rdx, %rax
    xorl %edx, %edx
    divq %rcx
    movq %rax, (%rsi)
    movq %rdx, %rax
    movl $1000000000, %r8d
    mulq %r8
    divq %rcx
    movq %rax, 8(%rsi)
    xorl %eax, %eax
    ret
3:
    movq $SYS_clock_gettime, %rax
    syscall
    ret
    .size __vdso_clock_gettime, .-__vdso_clock_gettime

    .weak clock_gettime
    .set clock_gettime, __vdso_clock_gettime

    .balign 16
    .globl __vdso_clock_getres
    .type __vdso_clock_getres, @function
__vdso_clock_getres:
    cmpl $CLOCK_MONOTONIC, %edi
    je 1f
    cmpl $CLOCK_MONOTONIC_RAW, %edi
    je 1f
    cmpl $CLOCK_MONOTONIC_COARSE, %edi
    je 1f
    cmpl $CLOCK_BOOTTIME, %edi
    je 1f
    cmpl $CLOCK_BOOTTIME_ALARM, %edi
    je 1f
    movq $SYS_clock_getres, %rax
    syscall
    ret
1:
    testq %rsi, %rsi
    je 2f
    movq $0, (%rsi)
    movq $1000, 8(%rsi)
2:
    xorl %eax, %eax
    ret
    .size __vdso_clock_getres, .-__vdso_clock_getres

    .weak clock_getres
    .set clock_getres, __vdso_clock_getres

    .section .note.GNU-stack,"",@progbits
