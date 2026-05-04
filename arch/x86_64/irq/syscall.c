/**
 * @file syscall.c
 * @brief x86_64 system call argument fetching and dispatch.
 *
 * On x86_64 (Linux convention), syscall arguments are passed in:
 *   arg0=RDI, arg1=RSI, arg2=RDX, arg3=R10, arg4=R8, arg5=R9
 * Syscall number in RAX, return value in RAX.
 * (R10 replaces RCX, which is clobbered by SYSCALL instruction.)
 */

#include "types.h"
#include "proc/thread.h"
#include "mm/vm.h"
#include "printf.h"
#include "defs.h"
#include "syscall.h"
#include "errno.h"
#include "param.h"
#include "string.h"
#include "seg.h"    /* wrmsr, rdmsr, MSR_FS_BASE */
#include "cmdline.h"

int fetchaddr(uint64 addr, uint64 *ip) {
    struct thread *p = current;
    // if (!p || !p->vm)
    //     return -1;
    return vm_copyin(p->vm, (char *)ip, addr, sizeof(*ip));
}

int fetchstr(uint64 addr, char *buf, int max) {
    struct thread *p = current;
    // if (!p || !p->vm)
    //     return -1;
    if (vm_copyinstr(p->vm, buf, addr, max) < 0)
        return -1;
    return strlen(buf);
}

/*
 * argraw — fetch raw syscall argument N from the trapframe.
 *
 * Linux x86-64 syscall convention:
 *   arg0 = rdi,  arg1 = rsi,  arg2 = rdx,
 *   arg3 = r10,  arg4 = r8,   arg5 = r9
 *
 * Note: r10 replaces rcx for the 4th argument because the SYSCALL
 * instruction clobbers rcx (loads user RIP into it).  The userspace
 * stub copies the C-convention rcx → r10 before issuing SYSCALL.
 *
 * rcx and r11 are "transaction registers" — clobbered by SYSCALL
 * hardware.  They are NOT used for argument passing.
 */
uint64 argraw(int n) {
    struct thread *p = current;
    switch (n) {
    case 0: return p->trapframe->trapframe.rdi;
    case 1: return p->trapframe->trapframe.rsi;
    case 2: return p->trapframe->trapframe.rdx;
    case 3: return p->trapframe->trapframe.r10;   /* r10, not rcx (clobbered) */
    case 4: return p->trapframe->trapframe.r8;
    case 5: return p->trapframe->trapframe.r9;
    }
    panic("argraw");
    return -1;
}

void argint(int n, int *ip) { *ip = argraw(n); }

void argint64(int n, int64 *ip) { *ip = argraw(n); }

void argaddr(int n, uint64 *ip) { *ip = argraw(n); }

int argstr(int n, char *buf, int max) {
    uint64 addr;
    argaddr(n, &addr);
    return fetchstr(addr, buf, max);
}

/* ── Syscall return value helpers ── */
static uint64 sys_ni_enosys(void) { return (uint64)-ENOSYS; }

/*
 * sys_arch_prctl — x86_64-specific: set/get FS/GS base.
 * Used by musl's __set_thread_area to set FS base for TLS.
 *
 * Arguments: code (RDI), addr (RSI).
 */
#define ARCH_SET_GS  0x1001
#define ARCH_SET_FS  0x1002
#define ARCH_GET_FS  0x1003
#define ARCH_GET_GS  0x1004

static uint64 sys_arch_prctl(void)
{
    int code;
    uint64 addr;
    argint(0, &code);
    argaddr(1, &addr);

    switch (code) {
    case ARCH_SET_FS:
        wrmsr(MSR_FS_BASE, addr);
        current->trapframe->tp = addr;  /* persist for context switch */
        return 0;
    case ARCH_SET_GS:
        /* Don't allow user to set GS — it's used for per-CPU data */
        return (uint64)-EPERM;
    case ARCH_GET_FS:
        if (vm_copyout(current->vm, addr, (char *)&(uint64){rdmsr(MSR_FS_BASE)}, sizeof(uint64)) < 0)
            return (uint64)-EFAULT;
        return 0;
    case ARCH_GET_GS:
        return (uint64)-EPERM;
    default:
        return (uint64)-EINVAL;
    }
}

/* ── Syscall handler prototypes ── */
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
extern uint64 sys_sync(void);
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
extern uint64 sys_statx(void);

// setitimer / getitimer
extern uint64 sys_setitimer(void);
extern uint64 sys_getitimer(void);

extern uint64 sys_kqueue(void);
extern uint64 sys_kevent_register(void);
extern uint64 sys_kevent_wait(void);
extern uint64 sys_epoll_create1(void);
extern uint64 sys_epoll_create(void);
extern uint64 sys_epoll_ctl(void);
extern uint64 sys_epoll_wait(void);
extern uint64 sys_epoll_pwait(void);
extern uint64 sys_epoll_pwait2(void);
extern uint64 sys_eventfd2(void);

// timerfd (timerfd.c)
extern uint64 sys_timerfd_create(void);
extern uint64 sys_timerfd_settime(void);
extern uint64 sys_timerfd_gettime(void);
extern uint64 sys_clock_nanosleep(void);

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
extern uint64 sys_get_robust_list(void);
extern uint64 sys_clock_settime(void);
extern uint64 sys_sched_rr_get_interval(void);
extern uint64 sys_sched_getaffinity(void);
extern uint64 sys_sched_setaffinity(void);
extern uint64 sys_sched_yield(void);
extern uint64 sys_sched_getscheduler(void);
extern uint64 sys_sched_setscheduler(void);
extern uint64 sys_sched_get_priority_max(void);
extern uint64 sys_sched_get_priority_min(void);
extern uint64 sys_rt_sigqueueinfo(void);
extern uint64 sys_clone3(void);

// Signal syscalls (proc/sys_signal.c)
extern uint64 sys_sigaltstack(void);

// VFS syscalls — fsync, fdatasync, utimensat, memfd_create (vfs/vfs_syscall.c)
extern uint64 sys_vfs_fsync(void);
extern uint64 sys_vfs_fdatasync(void);
extern uint64 sys_fadvise64(void);
extern uint64 sys_fallocate(void);
extern uint64 sys_utimensat(void);
extern uint64 sys_memfd_create(void);
extern uint64 sys_inotify_init1(void);
extern uint64 sys_inotify_add_watch(void);
extern uint64 sys_inotify_rm_watch(void);
extern uint64 sys_mlock2(void);
extern uint64 sys_mlockall(void);
extern uint64 sys_munlock(void);
extern uint64 sys_munlockall(void);
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

/* ── Syscall routing table (same as RISC-V, shared syscall numbers) ── */
static uint64 (*syscalls[])(void) = {
    [SYS_clone] sys_clone,
    [SYS_exit] sys_exit,
    [SYS_wait] sys_wait,
    [SYS_pipe] sys_vfs_pipe,
    [SYS_read] sys_vfs_read,
    [SYS_kill] sys_kill,
    [SYS_exec] sys_exec,
    [SYS_fstat] sys_vfs_fstat,
    [SYS_chdir] sys_vfs_chdir,
    [SYS_dup] sys_vfs_dup,
    [SYS_getpid] sys_getpid,
    [SYS_getppid] sys_getppid,
    [SYS_sbrk] sys_sbrk,
    [SYS_sleep] sys_sleep,
    [SYS_uptime] sys_uptime,
    [SYS_open] sys_vfs_open,
    [SYS_write] sys_vfs_write,
    [SYS_mknod] sys_vfs_mknod,
    [SYS_unlink] sys_vfs_unlink,
    [SYS_link] sys_vfs_link,
    [SYS_mkdir] sys_vfs_mkdir,
    [SYS_close] sys_vfs_close,
    [SYS_connect] sys_sconnect,    // socket connect (musl sends SYS_connect=36)
    [SYS_symlink] sys_vfs_symlink,
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
    [SYS_futex_x86] sys_futex,
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
    [SYS_arch_prctl] sys_arch_prctl,
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
    [SYS_sched_getaffinity] sys_sched_getaffinity,
    [SYS_sched_setaffinity] sys_sched_setaffinity,
    [SYS_sched_yield] sys_sched_yield,
    [SYS_sched_getscheduler] sys_sched_getscheduler,
    [SYS_sched_setscheduler] sys_sched_setscheduler,
    [SYS_sched_get_priority_max] sys_sched_get_priority_max,
    [SYS_sched_get_priority_min] sys_sched_get_priority_min,
#ifdef USE_LWIP
    [SYS_recvmmsg_time64] sys_recvmmsg,
#else
    [SYS_recvmmsg_time64] sys_ni_enosys,
#endif
    [SYS_pselect6_time64] sys_pselect6,
    [SYS_pselect6_time64_generic] sys_pselect6,
    [SYS_ppoll_time64] sys_vfs_ppoll,
    [SYS_futex_time64] sys_futex,
    [SYS_mq_timedsend_time64] sys_ni_enosys,
    [SYS_mq_timedreceive_time64] sys_ni_enosys,
    [SYS_eventfd2] sys_eventfd2,
    [SYS_eventfd2_x86] sys_eventfd2,
    [SYS_timerfd_create] sys_timerfd_create,
    [SYS_timerfd_create_x86] sys_timerfd_create,
    [SYS_timerfd_settime] sys_timerfd_settime,
    [SYS_timerfd_settime_x86] sys_timerfd_settime,
    [SYS_timerfd_settime64] sys_timerfd_settime,
    [SYS_timerfd_gettime] sys_timerfd_gettime,
    [SYS_timerfd_gettime_x86] sys_timerfd_gettime,
    [SYS_timerfd_gettime64] sys_timerfd_gettime,
    [SYS_clock_nanosleep] sys_clock_nanosleep,
    [SYS_clock_nanosleep_x86] sys_clock_nanosleep,
    [SYS_clock_nanosleep_time64] sys_clock_nanosleep,
    [SYS_epoll_pwait] sys_epoll_pwait,
    [SYS_epoll_pwait_x86] sys_epoll_pwait,
    [SYS_epoll_pwait2] sys_epoll_pwait2,
    [SYS_epoll_wait_x86] sys_epoll_wait,
    [SYS_epoll_ctl] sys_epoll_ctl,
    [SYS_epoll_ctl_x86] sys_epoll_ctl,
    [SYS_epoll_create1] sys_epoll_create1,
    [SYS_epoll_create1_x86] sys_epoll_create1,
    [SYS_epoll_create_x86] sys_epoll_create,
    [SYS_umask] sys_umask,
    [SYS_fchownat] sys_vfs_fchownat,
    [SYS_fchown] sys_vfs_fchown,
    [SYS_fchmodat] sys_vfs_fchmodat,
    [SYS_fchmod] sys_vfs_fchmod,
    [SYS_getitimer] sys_getitimer,
    [SYS_setitimer] sys_setitimer,
    [SYS_ppoll] sys_vfs_ppoll,
    [SYS_ppoll_x86] sys_vfs_ppoll,
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
    [SYS_get_robust_list] sys_get_robust_list,
    [SYS_get_robust_list_x86] sys_get_robust_list,
    [SYS_set_robust_list] sys_set_robust_list,
    [SYS_set_robust_list_x86] sys_set_robust_list,
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
    [SYS_statx] sys_statx,
    [SYS_inotify_init1] sys_ni_enosys,
    [SYS_inotify_add_watch] sys_ni_enosys,
    [SYS_inotify_rm_watch] sys_ni_enosys,
    /* Linux x86_64 native __NR_getrandom — OpenSSL direct call */
    [SYS_getrandom_x86] sys_getrandom,
    [SYS_memfd_create_x86] sys_memfd_create,
    [SYS_memfd_create_generic] sys_memfd_create,
    [SYS_membarrier] sys_membarrier,
    [SYS_fadvise64] sys_fadvise64,
    [SYS_fallocate] sys_fallocate,
    [SYS_poweroff] sys_poweroff,
    [SYS_reboot] sys_reboot,
    /* Resource limit syscalls (musl high numbers → prlimit64) */
    [SYS_getrlimit] sys_getrlimit,
    [SYS_setrlimit] sys_setrlimit,
    [SYS_prlimit64_musl] sys_prlimit64,
};

static int webkit_sysret_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_sysret_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int webkit_syswait_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_syswait_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int webkit_path_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_path_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int is_webkit_process_name(const char *name)
{
    return strncmp(name, "MiniBrowser", 11) == 0 ||
        strncmp(name, "WebKit", 6) == 0 ||
        strncmp(name, "wlcomp", 6) == 0;
}

static int is_webkit_browser_process_name(const char *name)
{
    return strncmp(name, "MiniBrowser", 11) == 0 ||
        strncmp(name, "WebKit", 6) == 0;
}

static int is_expected_transient_errno(int err)
{
    switch (err) {
    case EAGAIN:
    case EINTR:
    case EINPROGRESS:
    case ENOENT:
        return 1;
    default:
        return 0;
    }
}

static const char *webkit_syscall_name(int num)
{
    switch (num) {
    case SYS_open: return "open";
    case SYS_close: return "close";
    case SYS_read: return "read";
    case SYS_write: return "write";
    case SYS_fstat: return "fstat";
    case SYS_getdents: return "getdents";
    case SYS_lseek: return "lseek";
    case SYS_access: return "access";
    case SYS_readlink: return "readlink";
    case SYS_stat: return "stat";
    case SYS_lstat: return "lstat";
    case SYS_poll: return "poll";
    case SYS_mmap: return "mmap";
    case SYS_munmap: return "munmap";
    case SYS_mprotect: return "mprotect";
    case SYS_mremap: return "mremap";
    case SYS_msync: return "msync";
    case SYS_madvise: return "madvise";
    case SYS_nanosleep: return "nanosleep";
    case SYS_futex: return "futex";
    case SYS_futex_x86: return "futex";
    case SYS_openat: return "openat";
    case SYS_readv: return "readv";
    case SYS_pread64: return "pread64";
    case SYS_fstatat: return "fstatat";
    case SYS_readlinkat: return "readlinkat";
    case SYS_faccessat: return "faccessat";
    case SYS_sendto: return "sendto";
    case SYS_recvfrom: return "recvfrom";
    case SYS_sendmsg: return "sendmsg";
    case SYS_recvmsg: return "recvmsg";
    case SYS_kevent_wait: return "kevent_wait";
    case SYS_recvmmsg_time64: return "recvmmsg";
    case SYS_pselect6_time64: return "pselect6";
    case SYS_pselect6_time64_generic: return "pselect6";
    case SYS_ppoll: return "ppoll";
    case SYS_ppoll_x86: return "ppoll";
    case SYS_ppoll_time64: return "ppoll";
    case SYS_epoll_pwait: return "epoll_pwait";
    case SYS_epoll_pwait_x86: return "epoll_pwait";
    case SYS_epoll_pwait2: return "epoll_pwait2";
    case SYS_epoll_wait_x86: return "epoll_wait";
    case SYS_sendmmsg: return "sendmmsg";
    case SYS_statx: return "statx";
    default: return "?";
    }
}

static int is_webkit_wait_trace_syscall(int num)
{
    switch (num) {
    case SYS_open:
    case SYS_close:
    case SYS_read:
    case SYS_write:
    case SYS_fstat:
    case SYS_getdents:
    case SYS_lseek:
    case SYS_access:
    case SYS_readlink:
    case SYS_stat:
    case SYS_lstat:
    case SYS_poll:
    case SYS_mmap:
    case SYS_munmap:
    case SYS_mprotect:
    case SYS_mremap:
    case SYS_msync:
    case SYS_madvise:
    case SYS_nanosleep:
    case SYS_futex:
    case SYS_futex_x86:
    case SYS_openat:
    case SYS_readv:
    case SYS_pread64:
    case SYS_fstatat:
    case SYS_readlinkat:
    case SYS_faccessat:
    case SYS_sendto:
    case SYS_recvfrom:
    case SYS_sendmsg:
    case SYS_recvmsg:
    case SYS_kevent_wait:
    case SYS_recvmmsg_time64:
    case SYS_pselect6_time64:
    case SYS_pselect6_time64_generic:
    case SYS_ppoll:
    case SYS_ppoll_x86:
    case SYS_ppoll_time64:
    case SYS_epoll_pwait:
    case SYS_epoll_pwait_x86:
    case SYS_epoll_pwait2:
    case SYS_epoll_wait_x86:
    case SYS_sendmmsg:
    case SYS_statx:
        return 1;
    default:
        return 0;
    }
}

static uint64 webkit_path_arg(int num, uint64 a0, uint64 a1)
{
    switch (num) {
    case SYS_open:
    case SYS_access:
    case SYS_readlink:
    case SYS_stat:
    case SYS_lstat:
        return a0;
    case SYS_openat:
    case SYS_fstatat:
    case SYS_readlinkat:
    case SYS_faccessat:
    case SYS_statx:
        return a1;
    default:
        return 0;
    }
}

/*
 * syscall — dispatch system call.
 *
 * On x86_64: syscall number in RAX, return value in RAX.
 * Arguments: RDI, RSI, RDX, R10, R8, R9.
 */
void syscall(void) {
    struct thread *p = current;
    int num = (int)p->trapframe->trapframe.rax;
    uint64 a0 = p->trapframe->trapframe.rdi;
    uint64 a1 = p->trapframe->trapframe.rsi;
    uint64 a2 = p->trapframe->trapframe.rdx;
    uint64 a3 = p->trapframe->trapframe.r10;
    uint64 a4 = p->trapframe->trapframe.r8;
    uint64 a5 = p->trapframe->trapframe.r9;
    uint64 ret;
    int trace_wait = webkit_syswait_trace_enabled() &&
        is_webkit_process_name(p->name) && is_webkit_wait_trace_syscall(num);

    if (trace_wait) {
        printf("webkit-syswait: enter pid=%d tgid=%d name=%s sys=%d(%s) "
               "args=%lx,%lx,%lx,%lx,%lx,%lx\n",
               p->pid, p->tgid, p->name, num, webkit_syscall_name(num),
               a0, a1, a2, a3, a4, a5);
        if (webkit_path_trace_enabled() &&
            is_webkit_browser_process_name(p->name)) {
            uint64 path_arg = webkit_path_arg(num, a0, a1);
            if (path_arg != 0) {
                char path[160];
                if (fetchstr(path_arg, path, sizeof(path)) >= 0) {
                    printf("webkit-path: enter pid=%d name=%s sys=%d(%s) "
                           "path=\"%s\"\n",
                           p->pid, p->name, num, webkit_syscall_name(num),
                           path);
                } else {
                    printf("webkit-path: enter pid=%d name=%s sys=%d(%s) "
                           "path=<fault:%lx>\n",
                           p->pid, p->name, num, webkit_syscall_name(num),
                           path_arg);
                }
            }
        }
    }

    if (num > 0 && num < (int)NELEM(syscalls) && syscalls[num]) {
        p->trapframe->trapframe.rax = syscalls[num]();
    } else {
        printf("pid %d %s: unknown syscall %d\n", p->pid, p->name, num);
        p->trapframe->trapframe.rax = (uint64)-ENOSYS;
    }

    ret = p->trapframe->trapframe.rax;
    if (trace_wait) {
        printf("webkit-syswait: exit pid=%d tgid=%d name=%s sys=%d(%s) "
               "ret=%ld\n",
               p->pid, p->tgid, p->name, num, webkit_syscall_name(num),
               (int64)ret);
    }
    if (webkit_sysret_trace_enabled() && is_webkit_process_name(p->name) &&
        (int64)ret < 0 &&
        !is_expected_transient_errno((int)(-(int64)ret))) {
        printf("webkit-sysret: pid=%d tgid=%d name=%s sys=%d ret=%ld "
               "args=%lx,%lx,%lx,%lx\n",
               p->pid, p->tgid, p->name, num, (int64)ret, a0, a1, a2, a3);
    }
}
