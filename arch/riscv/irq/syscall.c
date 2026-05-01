#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "syscall.h"
#include "defs.h"
#include "printf.h"
#include <mm/vm.h>
#include "errno.h"

/* ============================================================
 * Syscall tracing — set STRACE_ENABLED to 1 to log every
 * syscall for processes matching STRACE_PROC_NAME.
 * Set STRACE_PROC_NAME to NULL to trace ALL processes.
 * ============================================================ */
#define STRACE_ENABLED   0
#define STRACE_PROC_NAME "python"   /* NULL = trace all */

#if STRACE_ENABLED
static const char *syscall_name(int num) {
    switch (num) {
    case 1: return "clone";
    case 2: return "vfork";
    case 3: return "exit";
    case 4: return "exit_group";
    case 5: return "wait";
    case 6: return "exec";
    case 7: return "kill";
    case 8: return "tgkill";
    case 9: return "getpid";
    case 10: return "gettid";
    case 11: return "sleep";
    case 12: return "pause";
    case 13: return "uptime";
    case 14: return "sbrk";
    case 15: return "setpgid";
    case 16: return "getpgid";
    case 17: return "setsid";
    case 18: return "getsid";
    case 19: return "getrandom";
    case 20: return "open";
    case 21: return "close";
    case 22: return "read";
    case 23: return "write";
    case 24: return "dup";
    case 25: return "pipe";
    case 26: return "fstat";
    case 27: return "link";
    case 28: return "unlink";
    case 29: return "symlink";
    case 30: return "mkdir";
    case 31: return "mknod";
    case 32: return "chdir";
    case 33: return "chroot";
    case 34: return "mount";
    case 35: return "umount";
    case 36: return "connect";
    case 37: return "getdents";
    case 38: return "getcwd";
    case 39: return "sync";
    case 40: return "ioctl";
    case 41: return "tcgetattr";
    case 42: return "tcsetattr";
    case 43: return "lseek";
    case 44: return "dup2";
    case 45: return "fcntl";
    case 46: return "access";
    case 47: return "rename";
    case 48: return "readlink";
    case 49: return "stat";
    case 50: return "mmap";
    case 51: return "munmap";
    case 52: return "mprotect";
    case 53: return "mremap";
    case 54: return "msync";
    case 55: return "mincore";
    case 56: return "madvise";
    case 57: return "gettimeofday";
    case 58: return "waitpid";
    case 59: return "nanosleep";
    case 60: return "ftruncate";
    case 61: return "getppid";
    case 62: return "uname";
    case 63: return "lstat";
    case 64: return "poll";
    case 65: return "kqueue";
    case 66: return "kevent_register";
    case 67: return "kevent_wait";
    case 68: return "brk";
    case 69: return "futex";
    case 70: return "sigaction";
    case 71: return "sigreturn";
    case 72: return "sigpending";
    case 73: return "sigprocmask";
    case 74: return "sigalarm";
    case 75: return "sigsuspend";
    case 76: return "sigwait";
    case 77: return "tkill";
    case 90: return "memstat";
    case 91: return "dumpproc";
    case 92: return "dumpchan";
    case 93: return "dumppcache";
    case 94: return "dumprq";
    case 95: return "kernbase";
    case 96: return "dumpinode";
    case 100: return "socket";
    case 101: return "bind";
    case 102: return "listen";
    case 103: return "accept";
    case 104: return "sconnect";
    case 105: return "sendto";
    case 106: return "recvfrom";
    case 107: return "setsockopt";
    case 108: return "getsockopt";
    case 109: return "shutdown";
    case 110: return "getpeername";
    case 111: return "getsockname";
    case 120: return "openat";
    case 121: return "writev";
    case 122: return "readv";
    case 123: return "set_tid_address";
    case 124: return "clock_gettime";
    case 125: return "clock_getres";
    case 126: return "pread64";
    case 127: return "pwrite64";
    case 128: return "fstatat";
    case 129: return "pipe2";
    case 130: return "mkdirat";
    case 131: return "mknodat";
    case 132: return "unlinkat";
    case 133: return "linkat";
    case 134: return "symlinkat";
    case 135: return "readlinkat";
    case 136: return "renameat";
    case 137: return "faccessat";
    case 138: return "dup3";
    case 139: return "getuid";
    case 140: return "geteuid";
    case 141: return "getgid";
    case 142: return "getegid";
    case 143: return "setuid";
    case 144: return "setgid";
    case 145: return "setreuid";
    case 146: return "setregid";
    case 147: return "getgroups";
    case 148: return "setgroups";
    case 158: return "arch_prctl";
    case 159: return "prlimit64";
    case 160: return "kstats";
    case 161: return "netconf";
    case 842: return "sendmmsg";
    case 847: return "umask";
    case 858: return "setresuid";
    case 859: return "setresgid";
    case 868: return "sched_rr_get_interval";
    case 872: return "recvmmsg";
    case 878: return "pselect6";
    case 903: return "getresuid";
    case 904: return "getresgid";
    case 939: return "fchownat";
    case 940: return "fchown";
    case 941: return "fchmodat";
    case 942: return "fchmod";
    case 943: return "getitimer";
    case 944: return "setitimer";
    case 945: return "ppoll";
    case 974: return "socketpair";
    case 908: return "fsync";
    case 915: return "fdatasync";
    case 866: return "set_robust_list";
    case 856: return "sigaltstack";
    case 950: return "prctl";
    case 851: return "sysinfo";
    case 902: return "getrusage";
    case 884: return "munlockall";
    case 885: return "munlock";
    case 892: return "mlockall";
    case 893: return "mlock2";
    case 894: return "memfd_create";
    case 998: return "membarrier";
    case 938: return "utimensat";
    case 937: return "clone3";
    case 935: return "clock_settime";
    case 162: return "preadv";
    case 163: return "pwritev";
    case 164: return "preadv2";
    case 165: return "pwritev2";
    case 166: return "poweroff";
    case 873: return "reboot";
    default: return "???";
    }
}

static inline int strace_match(struct thread *p) {
    const char *filter = STRACE_PROC_NAME;
    if (filter == NULL)
        return 1;
    return (strncmp(p->name, filter, strlen(filter)) == 0);
}
#endif /* STRACE_ENABLED */

static uint64 sys_ni_enosys(void) { return (uint64)-ENOSYS; }

// Fetch the uint64 at addr from the current thread.
int fetchaddr(uint64 addr, uint64 *ip) {
    struct thread *p = current;
    // if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in
    // case of overflow
    //   return -1;
    if (vm_copyin(p->vm, (char *)ip, addr, sizeof(*ip)) != 0)
        return -1;
    return 0;
}

// Fetch the nul-terminated string at addr from the current thread.
// Returns length of string, not including nul, or -1 for error.
int fetchstr(uint64 addr, char *buf, int max) {
    struct thread *p = current;
    if (vm_copyinstr(p->vm, buf, addr, max) < 0)
        return -1;
    return strlen(buf);
}

uint64 argraw(int n) {
    struct thread *p = current;
    switch (n) {
    case 0:
        return p->trapframe->trapframe.a0;
    case 1:
        return p->trapframe->trapframe.a1;
    case 2:
        return p->trapframe->trapframe.a2;
    case 3:
        return p->trapframe->trapframe.a3;
    case 4:
        return p->trapframe->trapframe.a4;
    case 5:
        return p->trapframe->trapframe.a5;
    }
    panic("argraw");
    return -1;
}

// Fetch the nth 32-bit system call argument.
void argint(int n, int *ip) { *ip = argraw(n); }

void argint64(int n, int64 *ip) { *ip = argraw(n); }

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void argaddr(int n, uint64 *ip) { *ip = argraw(n); }

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int argstr(int n, char *buf, int max) {
    uint64 addr;
    argaddr(n, &addr);
    return fetchstr(addr, buf, max);
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_clone(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_getpid(void);
extern uint64 sys_getppid(void);
extern uint64 sys_getuid(void);
extern uint64 sys_geteuid(void);
extern uint64 sys_getgid(void);
extern uint64 sys_getegid(void);
extern uint64 sys_setuid(void);
extern uint64 sys_setgid(void);
extern uint64 sys_setreuid(void);
extern uint64 sys_setregid(void);
extern uint64 sys_setresuid(void);
extern uint64 sys_setresgid(void);
extern uint64 sys_getresuid(void);
extern uint64 sys_getresgid(void);
extern uint64 sys_getgroups(void);
extern uint64 sys_setgroups(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_uptime(void);
extern uint64 sys_gettimeofday(void);
extern uint64 sys_waitpid(void);
extern uint64 sys_nanosleep(void);
extern uint64 sys_uname(void);
extern uint64 sys_sigaction(void);
extern uint64 sys_sigpending(void);
extern uint64 sys_sigprocmask(void);
extern uint64 sys_sigreturn(void);
// extern uint64 sys_sigalarm(void);
extern uint64 sys_pause(void);
extern uint64 sys_gettid(void);
extern uint64 sys_exit_group(void);
extern uint64 sys_tgkill(void);
extern uint64 sys_tkill(void);
extern uint64 sys_sigsuspend(void);
extern uint64 sys_sigwait(void);
extern uint64 sys_vfork(void);
extern uint64 sys_setpgid(void);
extern uint64 sys_getpgid(void);
extern uint64 sys_setsid(void);
extern uint64 sys_getsid(void);
extern uint64 sys_getrandom(void);
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
extern uint64 sys_mprotect(void);
extern uint64 sys_mremap(void);
extern uint64 sys_msync(void);
extern uint64 sys_mincore(void);
extern uint64 sys_madvise(void);

// Extended syscalls for musl support
extern uint64 sys_brk(void);
extern uint64 sys_futex(void);
extern uint64 sys_vfs_openat(void);
extern uint64 sys_vfs_writev(void);
extern uint64 sys_vfs_readv(void);
extern uint64 sys_set_tid_address(void);
extern uint64 sys_clock_gettime(void);
extern uint64 sys_clock_getres(void);
extern uint64 sys_vfs_pread64(void);
extern uint64 sys_vfs_pwrite64(void);
extern uint64 sys_vfs_preadv(void);
extern uint64 sys_vfs_pwritev(void);
extern uint64 sys_vfs_preadv2(void);
extern uint64 sys_vfs_pwritev2(void);
extern uint64 sys_vfs_fstatat(void);
extern uint64 sys_vfs_pipe2(void);

extern uint64 sys_memstat(void);
extern uint64 sys_dumpproc(void);
extern uint64 sys_dumpchan(void);
extern uint64 sys_dumppcache(void);
extern uint64 sys_dumprq(void);
extern uint64 sys_kernbase(void);
extern uint64 sys_dumpinode(void);
extern uint64 sys_dumpblk(void);
extern uint64 sys_losetup(void);

// Network / socket syscalls (lwip_port/sys_socket.c)
#ifdef USE_LWIP
extern uint64 sys_socket(void);
extern uint64 sys_bind(void);
extern uint64 sys_listen(void);
extern uint64 sys_accept(void);
extern uint64 sys_sconnect(void);
extern uint64 sys_sendto(void);
extern uint64 sys_recvfrom(void);
extern uint64 sys_setsockopt(void);
extern uint64 sys_getsockopt(void);
extern uint64 sys_shutdown(void);
extern uint64 sys_getpeername(void);
extern uint64 sys_getsockname(void);
extern uint64 sys_sendmsg(void);
extern uint64 sys_recvmsg(void);
extern uint64 sys_accept4(void);
extern uint64 sys_sendfile(void);
extern uint64 sys_socketpair(void);
extern uint64 sys_sendmmsg(void);
#endif

// 900
extern uint64 sys_sync(void);

// VFS syscalls (implementations in vfs_syscall.c)
extern uint64 sys_vfs_dup(void);
extern uint64 sys_vfs_read(void);
extern uint64 sys_vfs_write(void);
extern uint64 sys_vfs_close(void);
extern uint64 sys_vfs_fstat(void);
extern uint64 sys_vfs_open(void);
extern uint64 sys_vfs_lseek(void);
extern uint64 sys_vfs_dup2(void);
extern uint64 sys_vfs_fcntl(void);
extern uint64 sys_flock(void);
extern uint64 sys_fstatfs(void);
extern uint64 sys_statfs(void);
extern uint64 sys_vfs_access(void);
extern uint64 sys_vfs_rename(void);
extern uint64 sys_vfs_readlink(void);
extern uint64 sys_vfs_stat(void);
extern uint64 sys_vfs_lstat(void);
extern uint64 sys_vfs_ftruncate(void);
extern uint64 sys_vfs_mkdir(void);
extern uint64 sys_vfs_mknod(void);
extern uint64 sys_vfs_unlink(void);
extern uint64 sys_vfs_link(void);
extern uint64 sys_vfs_symlink(void);
extern uint64 sys_vfs_chdir(void);
extern uint64 sys_vfs_pipe(void);
extern uint64 sys_vfs_connect(void);
extern uint64 sys_getdents(void);
extern uint64 sys_chroot(void);
extern uint64 sys_mount(void);
extern uint64 sys_umount(void);
extern uint64 sys_getcwd(void);
extern uint64 sys_vfs_ioctl(void);
extern uint64 sys_tcgetattr(void);
extern uint64 sys_tcsetattr(void);
extern uint64 sys_vfs_poll(void);
extern uint64 sys_vfs_ppoll(void);
extern uint64 sys_pselect6(void);

// *at() variants for musl libc compatibility
extern uint64 sys_vfs_mkdirat(void);
extern uint64 sys_vfs_mknodat(void);
extern uint64 sys_vfs_unlinkat(void);
extern uint64 sys_vfs_linkat(void);
extern uint64 sys_vfs_symlinkat(void);
extern uint64 sys_vfs_readlinkat(void);
extern uint64 sys_vfs_renameat(void);
extern uint64 sys_vfs_faccessat(void);
extern uint64 sys_vfs_dup3(void);

// File ownership and permission syscalls
extern uint64 sys_vfs_fchmod(void);
extern uint64 sys_vfs_fchmodat(void);
extern uint64 sys_vfs_fchown(void);
extern uint64 sys_vfs_fchownat(void);
extern uint64 sys_umask(void);

// setitimer / getitimer
extern uint64 sys_setitimer(void);
extern uint64 sys_getitimer(void);

// kqueue syscalls
extern uint64 sys_kqueue(void);
extern uint64 sys_kevent_register(void);
extern uint64 sys_kevent_wait(void);

// epoll syscalls (wrappers over kqueue)
extern uint64 sys_epoll_create1(void);
extern uint64 sys_epoll_ctl(void);
extern uint64 sys_epoll_pwait(void);
extern uint64 sys_eventfd2(void);

// resource limits (accounting.c)
extern uint64 sys_prlimit64(void);
extern uint64 sys_getrlimit(void);
extern uint64 sys_setrlimit(void);
extern uint64 sys_kstats(void);

// network configuration (lwip_port/lwip_glue.c)
#ifdef USE_LWIP
extern uint64 sys_netconf(void);
#endif

// Miscellaneous process syscalls (proc/sys_misc.c)
extern uint64 sys_prctl(void);
extern uint64 sys_sysinfo(void);
extern uint64 sys_getrusage(void);
extern uint64 sys_getpriority(void);
extern uint64 sys_setpriority(void);
extern uint64 sys_set_robust_list(void);
extern uint64 sys_clock_settime(void);
extern uint64 sys_sched_rr_get_interval(void);
extern uint64 sys_rt_sigqueueinfo(void);
extern uint64 sys_clone3(void);
extern uint64 sys_mlock2(void);
extern uint64 sys_mlockall(void);
extern uint64 sys_munlock(void);
extern uint64 sys_munlockall(void);

// Signal syscalls (proc/sys_signal.c)
extern uint64 sys_sigaltstack(void);

// VFS syscalls — fsync, fdatasync, utimensat, memfd_create (vfs/vfs_syscall.c)
extern uint64 sys_vfs_fsync(void);
extern uint64 sys_vfs_fdatasync(void);
extern uint64 sys_fadvise64(void);
extern uint64 sys_fallocate(void);
extern uint64 sys_utimensat(void);
extern uint64 sys_memfd_create(void);
extern uint64 sys_membarrier(void);

// Power management (kernel/power.c)
extern uint64 sys_poweroff(void);
extern uint64 sys_reboot(void);

// System V IPC syscalls (ipc/*.c)
extern uint64 sys_shmget(void);
extern uint64 sys_shmat(void);
extern uint64 sys_shmdt(void);
extern uint64 sys_shmctl(void);
extern uint64 sys_semget(void);
extern uint64 sys_semop(void);
extern uint64 sys_semtimedop(void);
extern uint64 sys_semctl(void);
extern uint64 sys_msgget(void);
extern uint64 sys_msgsnd(void);
extern uint64 sys_msgrcv(void);
extern uint64 sys_msgctl(void);

// recvmmsg (lwip_port/sys_socket.c)
#ifdef USE_LWIP
extern uint64 sys_recvmmsg(void);
#endif

/*
 * Syscall routing table
 *
 * All file system syscalls (pipe, read, write, open, close, etc.) are now
 * routed to VFS implementations (sys_vfs_*). The legacy sysfile.c has been
 * removed from the build.
 *
 * VFS syscalls use:
 *   - vfs_fdtable for file descriptor management (replaces ofile[])
 *   - vfs_file for file operations (replaces struct file)
 *   - vfs_inode for inode operations (replaces struct inode)
 */
STATIC uint64 (*syscalls[])(void) = {
    [SYS_clone] sys_clone,
    [SYS_exit] sys_exit,
    [SYS_wait] sys_wait,
    [SYS_pipe] sys_vfs_pipe, // VFS
    [SYS_read] sys_vfs_read, // VFS
    [SYS_kill] sys_kill,
    [SYS_exec] sys_exec,
    [SYS_fstat] sys_vfs_fstat, // VFS
    [SYS_chdir] sys_vfs_chdir, // VFS
    [SYS_dup] sys_vfs_dup,     // VFS
    [SYS_getpid] sys_getpid,
    [SYS_getppid] sys_getppid,
    [SYS_sbrk] sys_sbrk,
    [SYS_sleep] sys_sleep,
    [SYS_uptime] sys_uptime,
    [SYS_open] sys_vfs_open,       // VFS
    [SYS_write] sys_vfs_write,     // VFS
    [SYS_mknod] sys_vfs_mknod,     // VFS
    [SYS_unlink] sys_vfs_unlink,   // VFS
    [SYS_link] sys_vfs_link,       // VFS
    [SYS_mkdir] sys_vfs_mkdir,     // VFS
    [SYS_close] sys_vfs_close,     // VFS
    [SYS_connect] sys_sconnect,    // socket connect (musl sends SYS_connect=36)
    [SYS_symlink] sys_vfs_symlink, // VFS
    // [SYS_sigalarm] sys_sigalarm,
    [SYS_sigaction] sys_sigaction,
    [SYS_sigreturn] sys_sigreturn,
    [SYS_sigpending] sys_sigpending,
    [SYS_sigprocmask] sys_sigprocmask,
    [SYS_pause] sys_pause,
    [SYS_sigsuspend] sys_sigsuspend,
    [SYS_sigwait] sys_sigwait,
    [SYS_tkill] sys_tkill,
    [SYS_gettid] sys_gettid,
    [SYS_exit_group] sys_exit_group,
    [SYS_tgkill] sys_tgkill,
    [SYS_vfork] sys_vfork,
    [SYS_setpgid] sys_setpgid,
    [SYS_getpgid] sys_getpgid,
    [SYS_setsid] sys_setsid,
    [SYS_getsid] sys_getsid,
    [SYS_getrandom] sys_getrandom,
    [SYS_mmap] sys_mmap,
    [SYS_munmap] sys_munmap,
    [SYS_mprotect] sys_mprotect,
    [SYS_mremap] sys_mremap,
    [SYS_msync] sys_msync,
    [SYS_mincore] sys_mincore,
    [SYS_madvise] sys_madvise,
    [SYS_brk] sys_brk,
    [SYS_futex] sys_futex,
    [SYS_memstat] sys_memstat,
    [SYS_dumpproc] sys_dumpproc,
    [SYS_dumpchan] sys_dumpchan,
    [SYS_dumppcache] sys_dumppcache,
    [SYS_dumprq] sys_dumprq,
    [SYS_kernbase] sys_kernbase,
    [SYS_dumpinode] sys_dumpinode,
    [SYS_dumpblk] sys_dumpblk,
    [SYS_losetup] sys_losetup,
    [SYS_sync] sys_sync,
    [SYS_ioctl] sys_vfs_ioctl,
    [SYS_tcgetattr] sys_tcgetattr,
    [SYS_tcsetattr] sys_tcsetattr,
    [SYS_lseek] sys_vfs_lseek,
    [SYS_dup2] sys_vfs_dup2,
    [SYS_fcntl] sys_vfs_fcntl,
    [SYS_flock] sys_flock,
    [SYS_fstatfs] sys_fstatfs,
    [SYS_statfs] sys_statfs,
    [SYS_access] sys_vfs_access,
    [SYS_rename] sys_vfs_rename,
    [SYS_readlink] sys_vfs_readlink,
    [SYS_stat] sys_vfs_stat,
    [SYS_lstat] sys_vfs_lstat,
    [SYS_poll] sys_vfs_poll,
    [SYS_kqueue] sys_kqueue,
    [SYS_kevent_register] sys_kevent_register,
    [SYS_kevent_wait] sys_kevent_wait,
    [SYS_ftruncate] sys_vfs_ftruncate,
    [SYS_gettimeofday] sys_gettimeofday,
    [SYS_waitpid] sys_waitpid,
    [SYS_nanosleep] sys_nanosleep,
    [SYS_uname] sys_uname,
    [SYS_getdents] sys_getdents,
    [SYS_chroot] sys_chroot,
    [SYS_mount] sys_mount,
    [SYS_umount] sys_umount,
    [SYS_getcwd] sys_getcwd,
    [SYS_openat] sys_vfs_openat,
    [SYS_writev] sys_vfs_writev,
    [SYS_readv] sys_vfs_readv,
    [SYS_set_tid_address] sys_set_tid_address,
    [SYS_clock_gettime] sys_clock_gettime,
    [SYS_clock_getres] sys_clock_getres,
    [SYS_pread64] sys_vfs_pread64,
    [SYS_pwrite64] sys_vfs_pwrite64,
    [SYS_preadv] sys_vfs_preadv,
    [SYS_pwritev] sys_vfs_pwritev,
    [SYS_preadv2] sys_vfs_preadv2,
    [SYS_pwritev2] sys_vfs_pwritev2,
    [SYS_fstatat] sys_vfs_fstatat,
    [SYS_pipe2] sys_vfs_pipe2,
    [SYS_mkdirat] sys_vfs_mkdirat,
    [SYS_mknodat] sys_vfs_mknodat,
    [SYS_unlinkat] sys_vfs_unlinkat,
    [SYS_linkat] sys_vfs_linkat,
    [SYS_symlinkat] sys_vfs_symlinkat,
    [SYS_readlinkat] sys_vfs_readlinkat,
    [SYS_renameat] sys_vfs_renameat,
    [SYS_faccessat] sys_vfs_faccessat,
    [SYS_dup3] sys_vfs_dup3,
    [SYS_getuid] sys_getuid,
    [SYS_geteuid] sys_geteuid,
    [SYS_getgid] sys_getgid,
    [SYS_getegid] sys_getegid,
    [SYS_setuid] sys_setuid,
    [SYS_setgid] sys_setgid,
    [SYS_setreuid] sys_setreuid,
    [SYS_setregid] sys_setregid,
    [SYS_setresuid] sys_setresuid,
    [SYS_setresgid] sys_setresgid,
    [SYS_getresuid] sys_getresuid,
    [SYS_getresgid] sys_getresgid,
    [SYS_getgroups] sys_getgroups,
    [SYS_setgroups] sys_setgroups,
    [SYS_prlimit64] sys_prlimit64,
    [SYS_kstats] sys_kstats,
#ifdef USE_LWIP
    [SYS_netconf] sys_netconf,
    [SYS_socket] sys_socket,
    [SYS_bind] sys_bind,
    [SYS_listen] sys_listen,
    [SYS_accept] sys_accept,
    [SYS_sconnect] sys_sconnect,
    [SYS_sendto] sys_sendto,
    [SYS_recvfrom] sys_recvfrom,
    [SYS_setsockopt] sys_setsockopt,
    [SYS_getsockopt] sys_getsockopt,
    [SYS_shutdown] sys_shutdown,
    [SYS_getpeername] sys_getpeername,
    [SYS_getsockname] sys_getsockname,
    [SYS_sendmsg] sys_sendmsg,
    [SYS_recvmsg] sys_recvmsg,
    [SYS_accept4] sys_accept4,
    [SYS_sendfile] sys_sendfile,
    [SYS_socketpair] sys_socketpair,
    [SYS_sendmmsg] sys_sendmmsg,
#endif
    [SYS_sched_rr_get_interval_time64] sys_sched_rr_get_interval,
#ifdef USE_LWIP
    [SYS_recvmmsg_time64] sys_recvmmsg,
#else
    [SYS_recvmmsg_time64] sys_ni_enosys,
#endif
    [SYS_pselect6_time64] sys_pselect6,
    [SYS_mq_timedsend_time64] sys_ni_enosys,
    [SYS_mq_timedreceive_time64] sys_ni_enosys,
    [SYS_eventfd2] sys_eventfd2,
    [SYS_epoll_pwait] sys_epoll_pwait,
    [SYS_epoll_ctl] sys_epoll_ctl,
    [SYS_epoll_create1] sys_epoll_create1,
    [SYS_umask] sys_umask,
    [SYS_fchownat] sys_vfs_fchownat,
    [SYS_fchown] sys_vfs_fchown,
    [SYS_fchmodat] sys_vfs_fchmodat,
    [SYS_fchmod] sys_vfs_fchmod,
    [SYS_getitimer] sys_getitimer,
    [SYS_setitimer] sys_setitimer,
    [SYS_ppoll] sys_vfs_ppoll,
    /* System V IPC */
    [SYS_semtimedop] sys_semtimedop,
    [SYS_shmget] sys_shmget,
    [SYS_shmdt] sys_shmdt,
    [SYS_shmctl] sys_shmctl,
    [SYS_shmat] sys_shmat,
    [SYS_semop] sys_semop,
    [SYS_semget] sys_semget,
    [SYS_semctl] sys_semctl,
    [SYS_msgsnd] sys_msgsnd,
    [SYS_msgrcv] sys_msgrcv,
    [SYS_msgget] sys_msgget,
    [SYS_msgctl] sys_msgctl,
    /* Process / VFS / signal syscalls */
    [SYS_utimensat] sys_utimensat,
    [SYS_clone3] sys_clone3,
    [SYS_rt_sigqueueinfo] sys_rt_sigqueueinfo,
    [SYS_clock_settime] sys_clock_settime,
    [SYS_fdatasync] sys_vfs_fdatasync,
    [SYS_fsync] sys_vfs_fsync,
    [SYS_set_robust_list] sys_set_robust_list,
    [SYS_sigaltstack] sys_sigaltstack,
    [SYS_prctl] sys_prctl,
    [SYS_sysinfo] sys_sysinfo,
    [SYS_getrusage] sys_getrusage,
    [SYS_getpriority] sys_getpriority,
    [SYS_setpriority] sys_setpriority,
    [SYS_munlockall] sys_munlockall,
    [SYS_munlock] sys_munlock,
    [SYS_mlockall] sys_mlockall,
    [SYS_mlock2] sys_mlock2,
    [SYS_memfd_create] sys_memfd_create,
    [SYS_membarrier] sys_membarrier,
    [SYS_fadvise64] sys_fadvise64,
    [SYS_fallocate] sys_fallocate,
    [SYS_poweroff] sys_poweroff,
    [SYS_reboot] sys_reboot,
    /* Resource limit syscalls (musl high numbers) */
    [SYS_getrlimit] sys_getrlimit,
    [SYS_setrlimit] sys_setrlimit,
    [SYS_prlimit64_musl] sys_prlimit64,
};

void syscall(void) {
    int num;
    struct thread *p = current;

    num = p->trapframe->trapframe.a7;

    if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
#if STRACE_ENABLED
        uint64 saved_a0 = p->trapframe->trapframe.a0;
        uint64 saved_a1 = p->trapframe->trapframe.a1;
        uint64 saved_a2 = p->trapframe->trapframe.a2;
        uint64 saved_a3 = p->trapframe->trapframe.a3;
#endif
        p->trapframe->trapframe.a0 = syscalls[num]();
#if STRACE_ENABLED
        if (strace_match(p)) {
            int64 ret = (int64)p->trapframe->trapframe.a0;
            /* Always log SQLite-critical syscalls (both success and error) */
            int is_sqlite_critical = (num == 26  /* fstat */
                                   || num == 45  /* fcntl */
                                   || num == 126 /* pread64 */
                                   || num == 127 /* pwrite64 */
                                   || num == 908 /* fsync */
                                   || num == 915 /* fdatasync */
                                   || num == 60  /* ftruncate */
                                   || num == 43  /* lseek */);
            if (is_sqlite_critical) {
                printf("[strace] pid %d %s(%d) = %ld fd=%ld a1=0x%lx a2=0x%lx a3=0x%lx\n",
                       p->pid, syscall_name(num), num, ret,
                       saved_a0, saved_a1, saved_a2, saved_a3);
            } else if (ret < 0 && ret > -4096) {
                printf("[strace] pid %d %s(%d) = %ld fd=%ld a1=0x%lx a2=0x%lx\n",
                       p->pid, syscall_name(num), num, ret,
                       saved_a0, saved_a1, saved_a2);
            }
        }
#endif
    } else {
#if STRACE_ENABLED
        if (strace_match(p))
            printf("strace: pid %d %s UNKNOWN syscall %d -> ENOSYS\n",
                   p->pid, p->name, num);
#endif
        p->trapframe->trapframe.a0 = (uint64)-ENOSYS;
    }
}
