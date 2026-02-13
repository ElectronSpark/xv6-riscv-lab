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
// 15-19 reserved

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
// 40-49 reserved

// --- Memory management (50-69) ---
#define SYS_mmap         50
#define SYS_munmap       51
#define SYS_mprotect     52
#define SYS_mremap       53
#define SYS_msync        54
#define SYS_mincore      55
#define SYS_madvise      56
// 57-69 reserved

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
// 97-99 reserved

#endif /* __KERNEL_SYSCALL_H */
