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
extern uint64 sys_kqueue(void);
extern uint64 sys_kevent_register(void);
extern uint64 sys_kevent_wait(void);
extern uint64 sys_epoll_create1(void);
extern uint64 sys_epoll_ctl(void);
extern uint64 sys_epoll_pwait(void);

// resource limits (accounting.c)
extern uint64 sys_prlimit64(void);
extern uint64 sys_kstats(void);

// network configuration (lwip_port/lwip_glue.c)
#ifdef USE_LWIP
extern uint64 sys_netconf(void);
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
    [SYS_sched_rr_get_interval_time64] sys_ni_enosys,
    [SYS_recvmmsg_time64] sys_ni_enosys,
    [SYS_pselect6_time64] sys_pselect6,
    [SYS_mq_timedsend_time64] sys_ni_enosys,
    [SYS_mq_timedreceive_time64] sys_ni_enosys,
    [SYS_epoll_pwait] sys_epoll_pwait,
    [SYS_epoll_ctl] sys_epoll_ctl,
    [SYS_epoll_create1] sys_epoll_create1,
    [SYS_umask] sys_umask,
    [SYS_fchownat] sys_vfs_fchownat,
    [SYS_fchown] sys_vfs_fchown,
    [SYS_fchmodat] sys_vfs_fchmodat,
    [SYS_fchmod] sys_vfs_fchmod,
};

/*
 * syscall — dispatch system call.
 *
 * On x86_64: syscall number in RAX, return value in RAX.
 * Arguments: RDI, RSI, RDX, R10, R8, R9.
 */
void syscall(void) {
    struct thread *p = current;
    int num = (int)p->trapframe->trapframe.rax;

    if (num > 0 && num < (int)NELEM(syscalls) && syscalls[num]) {
        p->trapframe->trapframe.rax = syscalls[num]();
    } else {
        printf("pid %d %s: unknown syscall %d\n", p->pid, p->name, num);
        p->trapframe->trapframe.rax = (uint64)-ENOSYS;
    }
}
