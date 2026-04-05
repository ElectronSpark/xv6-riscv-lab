#ifndef __USER_ABI_SYSCALL_H
#define __USER_ABI_SYSCALL_H

#include "compiler.h"

/*
 * System call numbers — Linux RISC-V (asm-generic) ABI compatible.
 *
 * Numbers match include/uapi/asm-generic/unistd.h for 64-bit.
 * xv6-specific syscalls that have no Linux equivalent use the
 * range 500+ to avoid conflicts with future Linux additions.
 *
 * Some xv6 syscall names map to different Linux names:
 *   open    -> openat (56)       link     -> linkat (37)
 *   unlink  -> unlinkat (35)     symlink  -> symlinkat (36)
 *   mkdir   -> mkdirat (34)      mknod    -> mknodat (33)
 *   pipe    -> pipe2 (59)        dup2     -> dup3 (24)
 *   access  -> faccessat (48)    readlink -> readlinkat (78)
 *   rename  -> renameat2 (276)   umount   -> umount2 (39)
 *   stat    -> newfstatat (79)   getdents -> getdents64 (61)
 *   sbrk    -> brk (214)        exec     -> execve (221)
 */

// --- File system / VFS ---
#define SYS_getcwd       17    // __NR_getcwd
#define SYS_dup          23    // __NR_dup
#define SYS_dup2         24    // __NR_dup3
#define SYS_fcntl        25    // __NR_fcntl
#define SYS_ioctl        29    // __NR_ioctl
#define SYS_mknod        33    // __NR_mknodat
#define SYS_mkdir        34    // __NR_mkdirat
#define SYS_unlink       35    // __NR_unlinkat
#define SYS_symlink      36    // __NR_symlinkat
#define SYS_link         37    // __NR_linkat
#define SYS_umount       39    // __NR_umount2
#define SYS_mount        40    // __NR_mount
#define SYS_statfs       43    // __NR_statfs
#define SYS_ftruncate    46    // __NR_ftruncate
#define SYS_access       48    // __NR_faccessat
#define SYS_chdir        49    // __NR_chdir
#define SYS_chroot       51    // __NR_chroot
#define SYS_open         56    // __NR_openat
#define SYS_close        57    // __NR_close
#define SYS_pipe         59    // __NR_pipe2
#define SYS_getdents     61    // __NR_getdents64
#define SYS_lseek        62    // __NR_lseek
#define SYS_read         63    // __NR_read
#define SYS_write        64    // __NR_write
#define SYS_readlink     78    // __NR_readlinkat
#define SYS_stat         79    // __NR_newfstatat
#define SYS_fstat        80    // __NR_fstat
#define SYS_sync         81    // __NR_sync
#define SYS_rename      276    // __NR_renameat2

// --- Process management ---
#define SYS_exit         93    // __NR_exit
#define SYS_exit_group   94    // __NR_exit_group
#define SYS_nanosleep   101    // __NR_nanosleep
#define SYS_kill        129    // __NR_kill
#define SYS_tkill       130    // __NR_tkill
#define SYS_tgkill      131    // __NR_tgkill
#define SYS_setpgid     154    // __NR_setpgid
#define SYS_getpgid     155    // __NR_getpgid
#define SYS_getsid      156    // __NR_getsid
#define SYS_setsid      157    // __NR_setsid
#define SYS_uname       160    // __NR_uname
#define SYS_gettimeofday 169   // __NR_gettimeofday
#define SYS_getpid      172    // __NR_getpid
#define SYS_getppid     173    // __NR_getppid
#define SYS_gettid      178    // __NR_gettid
#define SYS_clone       220    // __NR_clone
#define SYS_exec        221    // __NR_execve
#define SYS_getrandom   278    // __NR_getrandom

// --- Networking ---
#define SYS_connect     203    // __NR_connect

// --- Memory management ---
#define SYS_sbrk        214    // __NR_brk
#define SYS_munmap      215    // __NR_munmap
#define SYS_mremap      216    // __NR_mremap
#define SYS_mmap        222    // __NR_mmap
#define SYS_mprotect    226    // __NR_mprotect
#define SYS_msync       227    // __NR_msync
#define SYS_mincore     232    // __NR_mincore
#define SYS_madvise     233    // __NR_madvise

// --- Signals ---
#define SYS_sigsuspend  133    // __NR_rt_sigsuspend
#define SYS_sigaction   134    // __NR_rt_sigaction
#define SYS_sigprocmask 135    // __NR_rt_sigprocmask
#define SYS_sigpending  136    // __NR_rt_sigpending
#define SYS_sigreturn   139    // __NR_rt_sigreturn

// --- Polling ---
#define SYS_poll         73    // __NR_ppoll

// =========================================================
// xv6-specific syscalls (no Linux equivalent) — range 500+
// =========================================================
#define SYS_vfork       500
#define SYS_wait        501
#define SYS_waitpid     502
#define SYS_sleep       503
#define SYS_pause       504
#define SYS_uptime      505
#define SYS_tcgetattr   506
#define SYS_tcsetattr   507
#define SYS_sigalarm    508
#define SYS_sigwait     509
#define SYS_lstat       510

// --- Debug / introspection (xv6-specific) ---
#define SYS_memstat     511
#define SYS_dumpproc    512
#define SYS_dumpchan    513
#define SYS_dumppcache  514
#define SYS_dumprq      515
#define SYS_kernbase    516
#define SYS_dumpinode   517

#endif /* __USER_ABI_SYSCALL_H */
