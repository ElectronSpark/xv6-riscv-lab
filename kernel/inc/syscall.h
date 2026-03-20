#ifndef __KERNEL_SYSCALL_H
#define __KERNEL_SYSCALL_H

#include "compiler.h"

/*
 * System call numbers — grouped by subsystem with gaps for future use.
 *
 *   1-19   Process management
 *  20-49   File system / VFS
 *  50-69   Memory management
 *  70-89   Signals
 *  90-99   Debug / introspection
 */

// --- Process management (1-19) ---
#define SYS_clone         1
#define SYS_vfork         2
#define SYS_exit          3
#define SYS_exit_group    4
#define SYS_wait          5
#define SYS_exec          6
#define SYS_kill          7
#define SYS_tgkill        8
#define SYS_getpid        9
#define SYS_gettid       10
#define SYS_sleep        11
#define SYS_pause        12
#define SYS_uptime       13
#define SYS_sbrk         14
#define SYS_setpgid      15
#define SYS_getpgid      16
#define SYS_setsid       17
#define SYS_getsid       18
#define SYS_getrandom    19

// --- File system / VFS (20-49) ---
#define SYS_open         20
#define SYS_close        21
#define SYS_read         22
#define SYS_write        23
#define SYS_dup          24
#define SYS_pipe         25
#define SYS_fstat        26
#define SYS_link         27
#define SYS_unlink       28
#define SYS_symlink      29
#define SYS_mkdir        30
#define SYS_mknod        31
#define SYS_chdir        32
#define SYS_chroot       33
#define SYS_mount        34
#define SYS_umount       35
#define SYS_connect      36
#define SYS_getdents     37
#define SYS_getcwd       38
#define SYS_sync         39
#define SYS_ioctl        40
#define SYS_tcgetattr    41
#define SYS_tcsetattr    42
#define SYS_lseek        43
#define SYS_dup2         44
#define SYS_fcntl        45
#define SYS_access       46
#define SYS_rename       47
#define SYS_readlink     48
#define SYS_stat         49

// --- File system / VFS continued (50-69, mixed with memory) ---
#define SYS_mmap         50
#define SYS_munmap       51
#define SYS_mprotect     52
#define SYS_mremap       53
#define SYS_msync        54
#define SYS_mincore      55
#define SYS_madvise      56
#define SYS_gettimeofday 57
#define SYS_waitpid      58
#define SYS_nanosleep    59
#define SYS_ftruncate    60
#define SYS_getppid      61
#define SYS_uname        62
#define SYS_lstat        63
#define SYS_poll         64
#define SYS_kqueue       65
#define SYS_kevent_register 66
#define SYS_kevent_wait  67
#define SYS_brk          68
#define SYS_futex        69

// --- Signals (70-89) ---
#define SYS_sigaction    70
#define SYS_sigreturn    71
#define SYS_sigpending   72
#define SYS_sigprocmask  73
#define SYS_sigalarm     74
#define SYS_sigsuspend   75
#define SYS_sigwait      76
#define SYS_tkill        77
// 78-89 reserved

// --- Debug / introspection (90-99) ---
#define SYS_memstat      90
#define SYS_dumpproc     91
#define SYS_dumpchan     92
#define SYS_dumppcache   93
#define SYS_dumprq       94
#define SYS_kernbase     95
#define SYS_dumpinode    96
#define SYS_dumpblk      97
#define SYS_losetup      98
// 99 reserved

// --- Network / sockets (100-119) ---
#define SYS_socket       100
#define SYS_bind         101
#define SYS_listen       102
#define SYS_accept       103
#define SYS_sconnect     104   // "sconnect" to avoid clash with legacy SYS_connect
#define SYS_sendto       105
#define SYS_recvfrom     106
#define SYS_setsockopt   107
#define SYS_getsockopt   108
#define SYS_shutdown     109
#define SYS_getpeername  110
#define SYS_getsockname  111
#define SYS_sendmsg      112
#define SYS_recvmsg      113
#define SYS_accept4      114
#define SYS_sendfile     115
// 116-119 reserved

// --- Extended syscalls (120-139) ---
#define SYS_openat         120
#define SYS_writev         121
#define SYS_readv          122
#define SYS_set_tid_address 123
#define SYS_clock_gettime  124
#define SYS_clock_getres   125
#define SYS_pread64        126
#define SYS_pwrite64       127
#define SYS_fstatat        128
#define SYS_pipe2          129
#define SYS_mkdirat        130
#define SYS_mknodat        131
#define SYS_unlinkat       132
#define SYS_linkat         133
#define SYS_symlinkat      134
#define SYS_readlinkat     135
#define SYS_renameat       136
#define SYS_faccessat      137
#define SYS_dup3           138
#define SYS_getuid         139
#define SYS_geteuid        140
#define SYS_getgid         141
#define SYS_getegid        142
#define SYS_setuid         143
#define SYS_setgid         144
#define SYS_setreuid       145
#define SYS_setregid       146
#define SYS_getgroups      147
#define SYS_setgroups      148
// 149-157 reserved

// --- x86_64-specific syscalls ---
// musl's __set_thread_area hardcodes Linux's __NR_arch_prctl (158)
#define SYS_arch_prctl     158

// --- Resource limits ---
#define SYS_prlimit64      159

// --- Kernel statistics ---
#define SYS_kstats         160

// --- Network configuration ---
#define SYS_netconf        161

// --- Vectored positional I/O ---
#define SYS_preadv         162
#define SYS_pwritev        163
#define SYS_preadv2        164
#define SYS_pwritev2       165

// --- Power management ---
#define SYS_poweroff       166

// Max syscall number (update when adding new syscalls)
#define SYS_MAXNUM         166

// --- Linux time64 compatibility stubs used by musl on rv64 ---
// These are intentionally unsupported in xv6 and return -ENOSYS via
// dispatcher stubs to avoid unknown-syscall log spam.
#define SYS_sched_rr_get_interval_time64 868
#define SYS_recvmmsg_time64              872
#define SYS_pselect6_time64              878
#define SYS_mq_timedsend_time64          887
#define SYS_mq_timedreceive_time64       888

// --- Resource limit stubs (high musl numbers) ---
#define SYS_getrlimit       840
#define SYS_setrlimit       841

// --- Additional socket stubs (high musl numbers) ---
#define SYS_sendmmsg        842

// --- File ownership and permission syscalls (musl high numbers) ---
#define SYS_umask           847
#define SYS_fchownat        939
#define SYS_fchown          940
#define SYS_fchmodat        941
#define SYS_fchmod          942
#define SYS_setresuid       858
#define SYS_setresgid       859
#define SYS_getresuid       903
#define SYS_getresgid       904
#define SYS_getitimer       943
#define SYS_setitimer       944
#define SYS_ppoll           945

// --- IPC stubs (musl high numbers, all return -ENOSYS) ---
#define SYS_semtimedop      923
#define SYS_shmget          924
#define SYS_shmdt           925
#define SYS_shmctl          926
#define SYS_shmat           927
#define SYS_semop           928
#define SYS_semget          929
#define SYS_semctl          930
#define SYS_msgsnd          931
#define SYS_msgrcv          932
#define SYS_msgget          933
#define SYS_msgctl          934

// --- Other musl stubs (return -ENOSYS) ---
#define SYS_utimensat       938
#define SYS_clone3          937
#define SYS_rt_sigqueueinfo 936
#define SYS_clock_settime   935
#define SYS_fdatasync       915
#define SYS_fsync           908
#define SYS_set_robust_list 866
#define SYS_sigaltstack     856
#define SYS_prctl           950
#define SYS_sysinfo         851
#define SYS_getrusage       902
#define SYS_getpriority     905
#define SYS_setpriority     860
#define SYS_memfd_create    894

// --- Linux-native x86_64 __NR_getrandom (318) ---
// OpenSSL calls getrandom with the native Linux x86_64 number
// directly instead of xv6's custom SYS_getrandom (19).
#define SYS_getrandom_x86   318

#define SYS_socketpair      974

// --- Aliases for musl stub numbers (mapped to real kernel numbers) ---
// musl defines these at high numbers (975-993) as stubs.
// We keep the kernel using compact numbers (112-115) and remap the musl
// stubs in musl-xv6/arch/*/bits/syscall.h.in to match.

// --- epoll compatibility (mapped from musl stub numbers) ---
// musl's epoll_create1(), epoll_ctl(), epoll_pwait() issue these numbers.
// The kernel implements them as thin wrappers over kqueue.
#define SYS_epoll_pwait     986
#define SYS_epoll_ctl       987
#define SYS_epoll_create1   988
#define SYS_eventfd2        985
#define SYS_membarrier      998
#define SYS_fadvise64       989
#define SYS_prlimit64_musl  996

// --- Power management (high number for musl compat) ---
#define SYS_reboot          873

#endif /* __KERNEL_SYSCALL_H */
