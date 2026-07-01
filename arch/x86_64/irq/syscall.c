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
#include "memlayout.h"
#include "mm/vm.h"
#include "printf.h"
#include "defs.h"
#include "syscall.h"
#include "errno.h"
#include "param.h"
#include "string.h"
#include "seg.h"    /* wrmsr, rdmsr, MSR_FS_BASE */
#include "clone_flags.h"
#include "cmdline.h"
#include "proc/chrome_lifecycle.h"
#include "proc/sched.h"
#include "timer/timer.h"
#include "vfs/file.h"
#include "maple_tree.h"

#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME 0x00000080
#endif

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
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_waitid(void);
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
extern uint64 sys_setfsuid(void);
extern uint64 sys_setfsgid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_uptime(void);
extern uint64 sys_gettimeofday(void);
extern uint64 sys_time(void);
extern uint64 sys_waitpid(void);
extern uint64 sys_nanosleep(void);
extern uint64 sys_uname(void);
extern uint64 sys_sigaction(void);
extern uint64 sys_rt_sigaction(void);
extern uint64 sys_sigpending(void);
extern uint64 sys_rt_sigpending(void);
extern uint64 sys_sigprocmask(void);
extern uint64 sys_rt_sigprocmask(void);
extern uint64 sys_sigreturn(void);
extern uint64 sys_pause(void);
extern uint64 sys_gettid(void);
extern uint64 sys_exit_group(void);
extern uint64 sys_tgkill(void);
extern uint64 sys_tkill(void);
extern uint64 sys_sigsuspend(void);
extern uint64 sys_rt_sigsuspend(void);
extern uint64 sys_sigwait(void);
extern uint64 sys_rt_sigtimedwait(void);
extern uint64 sys_vfork(void);
extern uint64 sys_setpgid(void);
extern uint64 sys_getpgid(void);
extern uint64 sys_getpgrp(void);
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
extern uint64 sys_vfs_fchdir(void);
extern uint64 sys_vfs_pipe(void);
extern uint64 sys_vfs_connect(void);
extern uint64 sys_getdents(void);
extern uint64 sys_getdents_compat(void);
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
extern uint64 sys_select(void);
extern uint64 sys_vfs_mkdirat(void);
extern uint64 sys_vfs_mknodat(void);
extern uint64 sys_vfs_unlinkat(void);
extern uint64 sys_vfs_linkat(void);
extern uint64 sys_vfs_symlinkat(void);
extern uint64 sys_vfs_readlinkat(void);
extern uint64 sys_vfs_renameat(void);
extern uint64 sys_vfs_faccessat(void);
extern uint64 sys_vfs_dup3(void);
extern uint64 sys_vfs_openat2(void);
extern uint64 sys_vfs_close_range(void);
extern uint64 sys_vfs_copy_file_range(void);
extern uint64 sys_vfs_xattr_not_supported(void);
extern uint64 sys_vfs_truncate(void);
extern uint64 sys_vfs_creat(void);
extern uint64 sys_vfs_rmdir(void);
extern uint64 sys_vfs_chmod(void);
extern uint64 sys_vfs_chown(void);
extern uint64 sys_vfs_lchown(void);

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
extern uint64 sys_alarm(void);

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
extern uint64 sys_eventfd(void);
extern uint64 sys_signalfd(void);
extern uint64 sys_signalfd4(void);

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
extern uint64 sys_kstatsctl(void);

// network configuration (lwip_port/lwip_glue.c)
#ifdef USE_LWIP
extern uint64 sys_netconf(void);
#endif

// Miscellaneous process syscalls (proc/sys_misc.c)
extern uint64 sys_prctl(void);
extern uint64 sys_sysinfo(void);
extern uint64 sys_getrusage(void);
extern uint64 sys_times(void);
extern uint64 sys_getpriority(void);
extern uint64 sys_setpriority(void);
extern uint64 sys_getcpu(void);
extern uint64 sys_rseq(void);
extern uint64 sys_capget(void);
extern uint64 sys_capset(void);
extern uint64 sys_set_robust_list(void);
extern uint64 sys_get_robust_list(void);
extern uint64 sys_clock_settime(void);
extern uint64 sys_sched_rr_get_interval(void);
extern uint64 sys_sched_getaffinity(void);
extern uint64 sys_sched_setaffinity(void);
extern uint64 sys_sched_yield(void);
extern uint64 sys_sched_getscheduler(void);
extern uint64 sys_sched_setscheduler(void);
extern uint64 sys_sched_getparam(void);
extern uint64 sys_sched_setparam(void);
extern uint64 sys_sched_getattr(void);
extern uint64 sys_sched_setattr(void);
extern uint64 sys_ioprio_get(void);
extern uint64 sys_ioprio_set(void);
extern uint64 sys_sched_get_priority_max(void);
extern uint64 sys_sched_get_priority_min(void);
extern uint64 sys_rt_sigqueueinfo(void);
extern uint64 sys_clone3(void);
extern uint64 sys_ptrace(void);
extern uint64 sys_syslog(void);
extern uint64 sys_uselib(void);
extern uint64 sys_personality(void);
extern uint64 sys_ustat(void);
extern uint64 sys_sysfs(void);
extern uint64 sys_vhangup(void);
extern uint64 sys_modify_ldt(void);
extern uint64 sys_pivot_root(void);
extern uint64 sys__sysctl(void);
extern uint64 sys_adjtimex(void);
extern uint64 sys_acct(void);
extern uint64 sys_settimeofday(void);
extern uint64 sys_swapon(void);
extern uint64 sys_swapoff(void);
extern uint64 sys_sethostname(void);
extern uint64 sys_setdomainname(void);
extern uint64 sys_iopl(void);
extern uint64 sys_ioperm(void);
extern uint64 sys_create_module(void);
extern uint64 sys_init_module(void);
extern uint64 sys_delete_module(void);
extern uint64 sys_get_kernel_syms(void);
extern uint64 sys_query_module(void);
extern uint64 sys_quotactl(void);
extern uint64 sys_nfsservctl(void);
extern uint64 sys_getpmsg(void);
extern uint64 sys_putpmsg(void);
extern uint64 sys_afs_syscall(void);
extern uint64 sys_tuxcall(void);
extern uint64 sys_security(void);
extern uint64 sys_set_thread_area(void);
extern uint64 sys_io_setup(void);
extern uint64 sys_io_destroy(void);
extern uint64 sys_io_getevents(void);
extern uint64 sys_io_submit(void);
extern uint64 sys_io_cancel(void);
extern uint64 sys_get_thread_area(void);
extern uint64 sys_lookup_dcookie(void);
extern uint64 sys_epoll_ctl_old(void);
extern uint64 sys_epoll_wait_old(void);
extern uint64 sys_remap_file_pages(void);
extern uint64 sys_restart_syscall(void);
extern uint64 sys_timer_create(void);
extern uint64 sys_timer_settime(void);
extern uint64 sys_timer_gettime(void);
extern uint64 sys_timer_getoverrun(void);
extern uint64 sys_timer_delete(void);
extern uint64 sys_vserver(void);
extern uint64 sys_mbind(void);
extern uint64 sys_set_mempolicy(void);
extern uint64 sys_get_mempolicy(void);
extern uint64 sys_mq_open(void);
extern uint64 sys_mq_unlink(void);
extern uint64 sys_mq_timedsend(void);
extern uint64 sys_mq_timedreceive(void);
extern uint64 sys_mq_notify(void);
extern uint64 sys_mq_getsetattr(void);
extern uint64 sys_kexec_load(void);
extern uint64 sys_add_key(void);
extern uint64 sys_request_key(void);
extern uint64 sys_keyctl(void);
extern uint64 sys_migrate_pages(void);
extern uint64 sys_unshare(void);
extern uint64 sys_splice(void);
extern uint64 sys_tee(void);
extern uint64 sys_vmsplice(void);
extern uint64 sys_move_pages(void);
extern uint64 sys_rt_tgsigqueueinfo(void);
extern uint64 sys_perf_event_open(void);
extern uint64 sys_fanotify_init(void);
extern uint64 sys_fanotify_mark(void);
extern uint64 sys_name_to_handle_at(void);
extern uint64 sys_open_by_handle_at(void);
extern uint64 sys_clock_adjtime(void);
extern uint64 sys_setns(void);
extern uint64 sys_process_vm_readv(void);
extern uint64 sys_process_vm_writev(void);
extern uint64 sys_kcmp(void);
extern uint64 sys_finit_module(void);
extern uint64 sys_seccomp(void);
extern uint64 sys_kexec_file_load(void);
extern uint64 sys_bpf(void);
extern uint64 sys_execveat(void);
extern uint64 sys_userfaultfd(void);
extern uint64 sys_pkey_mprotect(void);
extern uint64 sys_pkey_alloc(void);
extern uint64 sys_pkey_free(void);
extern uint64 sys_io_pgetevents(void);
extern uint64 sys_pidfd_send_signal(void);
extern uint64 sys_io_uring_setup(void);
extern uint64 sys_io_uring_enter(void);
extern uint64 sys_io_uring_register(void);
extern uint64 sys_open_tree(void);
extern uint64 sys_move_mount(void);
extern uint64 sys_fsopen(void);
extern uint64 sys_fsconfig(void);
extern uint64 sys_fsmount(void);
extern uint64 sys_fspick(void);
extern uint64 sys_pidfd_open(void);
extern uint64 sys_pidfd_getfd(void);
extern uint64 sys_process_madvise(void);
extern uint64 sys_mount_setattr(void);
extern uint64 sys_quotactl_fd(void);
extern uint64 sys_landlock_create_ruleset(void);
extern uint64 sys_landlock_add_rule(void);
extern uint64 sys_landlock_restrict_self(void);
extern uint64 sys_memfd_secret(void);
extern uint64 sys_process_mrelease(void);
extern uint64 sys_futex_waitv(void);
extern uint64 sys_set_mempolicy_home_node(void);
extern uint64 sys_cachestat(void);
extern uint64 sys_map_shadow_stack(void);
extern uint64 sys_futex_wake(void);
extern uint64 sys_futex_wait(void);
extern uint64 sys_futex_requeue(void);
extern uint64 sys_statmount(void);
extern uint64 sys_listmount(void);
extern uint64 sys_lsm_get_self_attr(void);
extern uint64 sys_lsm_set_self_attr(void);
extern uint64 sys_lsm_list_modules(void);

// Signal syscalls (proc/sys_signal.c)
extern uint64 sys_sigaltstack(void);

// VFS syscalls — fsync, fdatasync, utimensat, memfd_create (vfs/vfs_syscall.c)
extern uint64 sys_vfs_fsync(void);
extern uint64 sys_vfs_fdatasync(void);
extern uint64 sys_fadvise64(void);
extern uint64 sys_vfs_readahead(void);
extern uint64 sys_vfs_sync_file_range(void);
extern uint64 sys_vfs_syncfs(void);
extern uint64 sys_fallocate(void);
extern uint64 sys_utimensat(void);
extern uint64 sys_futimesat(void);
extern uint64 sys_utime(void);
extern uint64 sys_utimes(void);
extern uint64 sys_memfd_create(void);
extern uint64 sys_inotify_init1(void);
extern uint64 sys_inotify_init(void);
extern uint64 sys_inotify_add_watch(void);
extern uint64 sys_inotify_rm_watch(void);
extern uint64 sys_mlock2(void);
extern uint64 sys_mlock(void);
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
    [SYS_clone_x86] sys_clone,
    [SYS_fork_x86] sys_fork,
    [SYS_exit] sys_exit,
    [SYS_exit_x86] sys_exit,
    [SYS_wait] sys_wait,
    [SYS_wait4_x86] sys_waitpid,
    [SYS_waitid_x86] sys_waitid,
    [SYS_pipe] sys_vfs_pipe,
    [SYS_pipe_x86] sys_vfs_pipe,
    [SYS_select_x86] sys_select,
    [SYS_read] sys_vfs_read,
    [SYS_read_x86] sys_vfs_read,
    [SYS_kill] sys_kill,
    [SYS_kill_x86] sys_kill,
    [SYS_tkill_x86] sys_tkill,
    [SYS_tgkill_x86] sys_tgkill,
    [SYS_exec] sys_exec,
    [SYS_execve_x86] sys_exec,
    [SYS_fstat] sys_vfs_fstat,
    [SYS_fstat_x86] sys_vfs_fstat,
    [SYS_chdir] sys_vfs_chdir,
    [SYS_chdir_x86] sys_vfs_chdir,
    [SYS_fchdir_x86] sys_vfs_fchdir,
    [SYS_dup] sys_vfs_dup,
    [SYS_dup_x86] sys_vfs_dup,
    [SYS_getpid] sys_getpid,
    [SYS_getpid_x86] sys_getpid,
    [SYS_gettid_x86] sys_gettid,
    [SYS_getppid] sys_getppid,
    [SYS_getppid_x86] sys_getppid,
    [SYS_sbrk] sys_sbrk,
    [SYS_sleep] sys_sleep,
    [SYS_uptime] sys_uptime,
    [SYS_open] sys_vfs_open,
    [SYS_open_x86] sys_vfs_open,
    [SYS_write] sys_vfs_write,
    [SYS_write_x86] sys_vfs_write,
    [SYS_mknod] sys_vfs_mknod,
    [SYS_mknod_x86] sys_vfs_mknod,
    [SYS_unlink] sys_vfs_unlink,
    [SYS_unlink_x86] sys_vfs_unlink,
    [SYS_rmdir_x86] sys_vfs_rmdir,
    [SYS_link] sys_vfs_link,
    [SYS_link_x86] sys_vfs_link,
    [SYS_mkdir] sys_vfs_mkdir,
    [SYS_mkdir_x86] sys_vfs_mkdir,
    [SYS_close] sys_vfs_close,
    [SYS_close_x86] sys_vfs_close,
    [SYS_connect] sys_sconnect,    // socket connect (musl sends SYS_connect=36)
    [SYS_symlink] sys_vfs_symlink,
    [SYS_symlink_x86] sys_vfs_symlink,
    [SYS_sigaction] sys_sigaction,
    [SYS_rt_sigaction_x86] sys_rt_sigaction,
    [SYS_sigreturn] sys_sigreturn,
    [SYS_rt_sigreturn_x86] sys_sigreturn,
    [SYS_sigpending] sys_sigpending,
    [SYS_rt_sigpending_x86] sys_rt_sigpending,
    [SYS_sigprocmask] sys_sigprocmask,
    [SYS_rt_sigprocmask_x86] sys_rt_sigprocmask,
    [SYS_pause] sys_pause,
    [SYS_pause_x86] sys_pause,
    [SYS_sigsuspend] sys_sigsuspend,
    [SYS_rt_sigsuspend_x86] sys_rt_sigsuspend,
    [SYS_sigwait] sys_sigwait,
    [SYS_rt_sigtimedwait_x86] sys_rt_sigtimedwait,
    [SYS_tkill] sys_tkill,
    [SYS_gettid] sys_gettid,
    [SYS_exit_group] sys_exit_group,
    [SYS_exit_group_x86] sys_exit_group,
    [SYS_tgkill] sys_tgkill,
    [SYS_vfork] sys_vfork,
    [SYS_vfork_x86] sys_vfork,
    [SYS_setpgid] sys_setpgid,
    [SYS_setpgid_x86] sys_setpgid,
    [SYS_getpgid] sys_getpgid,
    [SYS_getpgid_x86] sys_getpgid,
    [SYS_getpgrp_x86] sys_getpgrp,
    [SYS_setsid] sys_setsid,
    [SYS_setsid_x86] sys_setsid,
    [SYS_getsid] sys_getsid,
    [SYS_getsid_x86] sys_getsid,
    [SYS_getrandom] sys_getrandom,
    [SYS_mmap] sys_mmap,
    [SYS_mmap_x86] sys_mmap,
    [SYS_munmap] sys_munmap,
    [SYS_munmap_x86] sys_munmap,
    [SYS_mprotect] sys_mprotect,
    [SYS_mprotect_x86] sys_mprotect,
    [SYS_mremap] sys_mremap,
    [SYS_mremap_x86] sys_mremap,
    [SYS_msync] sys_msync,
    [SYS_msync_x86] sys_msync,
    [SYS_mincore] sys_mincore,
    [SYS_mincore_x86] sys_mincore,
    [SYS_madvise] sys_madvise,
    [SYS_madvise_x86] sys_madvise,
    [SYS_brk] sys_brk,
    [SYS_brk_x86] sys_brk,
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
    [SYS_sync_x86] sys_sync,
    [SYS_ioctl] sys_vfs_ioctl,
    [SYS_ioctl_x86] sys_vfs_ioctl,
    [SYS_tcgetattr] sys_tcgetattr,
    [SYS_tcsetattr] sys_tcsetattr,
    [SYS_lseek] sys_vfs_lseek,
    [SYS_lseek_x86] sys_vfs_lseek,
    [SYS_dup2] sys_vfs_dup2,
    [SYS_dup2_x86] sys_vfs_dup2,
    [SYS_fcntl] sys_vfs_fcntl,
    [SYS_fcntl_x86] sys_vfs_fcntl,
    [SYS_flock] sys_flock,
    [SYS_flock_x86] sys_flock,
    [SYS_fstatfs] sys_fstatfs,
    [SYS_fstatfs_x86] sys_fstatfs,
    [SYS_statfs] sys_statfs,
    [SYS_statfs_x86] sys_statfs,
    [SYS_access] sys_vfs_access,
    [SYS_access_x86] sys_vfs_access,
    [SYS_rename] sys_vfs_rename,
    [SYS_rename_x86] sys_vfs_rename,
    [SYS_readlink] sys_vfs_readlink,
    [SYS_readlink_x86] sys_vfs_readlink,
    [SYS_stat] sys_vfs_stat,
    [SYS_stat_x86] sys_vfs_stat,
    [SYS_lstat] sys_vfs_lstat,
    [SYS_lstat_x86] sys_vfs_lstat,
    [SYS_poll] sys_vfs_poll,
    [SYS_poll_x86] sys_vfs_poll,
    [SYS_kqueue] sys_kqueue,
    [SYS_kevent_register] sys_kevent_register,
    [SYS_kevent_wait] sys_kevent_wait,
    [SYS_ftruncate] sys_vfs_ftruncate,
    [SYS_ftruncate_x86] sys_vfs_ftruncate,
    [SYS_truncate_x86] sys_vfs_truncate,
    [SYS_creat_x86] sys_vfs_creat,
    [SYS_gettimeofday] sys_gettimeofday,
    [SYS_gettimeofday_x86] sys_gettimeofday,
    [SYS_time_x86] sys_time,
    [SYS_waitpid] sys_waitpid,
    [SYS_nanosleep] sys_nanosleep,
    [SYS_nanosleep_x86] sys_nanosleep,
    [SYS_uname] sys_uname,
    [SYS_uname_x86] sys_uname,
    [SYS_getdents] sys_getdents,
    [SYS_getdents_x86] sys_getdents_compat,
    [SYS_getdents64_x86] sys_getdents,
    [SYS_chroot] sys_chroot,
    [SYS_chroot_x86] sys_chroot,
    [SYS_mount] sys_mount,
    [SYS_mount_x86] sys_mount,
    [SYS_umount] sys_umount,
    [SYS_umount2_x86] sys_umount,
    [SYS_getcwd] sys_getcwd,
    [SYS_getcwd_x86] sys_getcwd,
    [SYS_openat] sys_vfs_openat,
    [SYS_openat_x86] sys_vfs_openat,
    [SYS_openat2_x86] sys_vfs_openat2,
    [SYS_writev] sys_vfs_writev,
    [SYS_writev_x86] sys_vfs_writev,
    [SYS_readv] sys_vfs_readv,
    [SYS_readv_x86] sys_vfs_readv,
    [SYS_set_tid_address] sys_set_tid_address,
    [SYS_set_tid_address_x86] sys_set_tid_address,
    [SYS_clock_gettime] sys_clock_gettime,
    [SYS_clock_getres] sys_clock_getres,
    [SYS_clock_settime_x86] sys_clock_settime,
    [SYS_clock_gettime_x86] sys_clock_gettime,
    [SYS_clock_getres_x86] sys_clock_getres,
    [SYS_pread64] sys_vfs_pread64,
    [SYS_pread64_x86] sys_vfs_pread64,
    [SYS_pwrite64] sys_vfs_pwrite64,
    [SYS_pwrite64_x86] sys_vfs_pwrite64,
    [SYS_preadv] sys_vfs_preadv,
    [SYS_preadv_x86] sys_vfs_preadv,
    [SYS_pwritev] sys_vfs_pwritev,
    [SYS_pwritev_x86] sys_vfs_pwritev,
    [SYS_preadv2] sys_vfs_preadv2,
    [SYS_preadv2_x86] sys_vfs_preadv2,
    [SYS_pwritev2] sys_vfs_pwritev2,
    [SYS_pwritev2_x86] sys_vfs_pwritev2,
    [SYS_fstatat] sys_vfs_fstatat,
    [SYS_newfstatat_x86] sys_vfs_fstatat,
    [SYS_pipe2] sys_vfs_pipe2,
    [SYS_pipe2_x86] sys_vfs_pipe2,
    [SYS_mkdirat] sys_vfs_mkdirat,
    [SYS_mkdirat_x86] sys_vfs_mkdirat,
    [SYS_mknodat] sys_vfs_mknodat,
    [SYS_mknodat_x86] sys_vfs_mknodat,
    [SYS_unlinkat] sys_vfs_unlinkat,
    [SYS_unlinkat_x86] sys_vfs_unlinkat,
    [SYS_linkat] sys_vfs_linkat,
    [SYS_linkat_x86] sys_vfs_linkat,
    [SYS_symlinkat] sys_vfs_symlinkat,
    [SYS_symlinkat_x86] sys_vfs_symlinkat,
    [SYS_readlinkat] sys_vfs_readlinkat,
    [SYS_readlinkat_x86] sys_vfs_readlinkat,
    [SYS_renameat] sys_vfs_renameat,
    [SYS_renameat_x86] sys_vfs_renameat,
    [SYS_renameat2_x86] sys_vfs_renameat,
    [SYS_faccessat] sys_vfs_faccessat,
    [SYS_faccessat_x86] sys_vfs_faccessat,
    [SYS_faccessat2_x86] sys_vfs_faccessat,
    [SYS_dup3] sys_vfs_dup3,
    [SYS_dup3_x86] sys_vfs_dup3,
    [SYS_close_range_x86] sys_vfs_close_range,
    [SYS_getuid] sys_getuid,
    [SYS_getuid_x86] sys_getuid,
    [SYS_geteuid] sys_geteuid,
    [SYS_geteuid_x86] sys_geteuid,
    [SYS_getgid] sys_getgid,
    [SYS_getgid_x86] sys_getgid,
    [SYS_getegid] sys_getegid,
    [SYS_getegid_x86] sys_getegid,
    [SYS_setuid] sys_setuid,
    [SYS_setuid_x86] sys_setuid,
    [SYS_setgid] sys_setgid,
    [SYS_setgid_x86] sys_setgid,
    [SYS_setreuid] sys_setreuid,
    [SYS_setreuid_x86] sys_setreuid,
    [SYS_setregid] sys_setregid,
    [SYS_setregid_x86] sys_setregid,
    [SYS_setresuid] sys_setresuid,
    [SYS_setresuid_x86] sys_setresuid,
    [SYS_setresgid] sys_setresgid,
    [SYS_setresgid_x86] sys_setresgid,
    [SYS_getresuid] sys_getresuid,
    [SYS_getresuid_x86] sys_getresuid,
    [SYS_getresgid] sys_getresgid,
    [SYS_getresgid_x86] sys_getresgid,
    [SYS_getgroups] sys_getgroups,
    [SYS_getgroups_x86] sys_getgroups,
    [SYS_setgroups] sys_setgroups,
    [SYS_setgroups_x86] sys_setgroups,
    [SYS_setfsuid_x86] sys_setfsuid,
    [SYS_setfsgid_x86] sys_setfsgid,
    [SYS_capget_x86] sys_capget,
    [SYS_capset_x86] sys_capset,
    [SYS_arch_prctl] sys_arch_prctl,
    [SYS_prlimit64] sys_prlimit64,
    [SYS_prlimit64_x86] sys_prlimit64,
    [SYS_kstats] sys_kstats,
    [SYS_kstatsctl] sys_kstatsctl,
#ifdef USE_LWIP
    [SYS_netconf] sys_netconf,
    [SYS_socket] sys_socket,
    [SYS_socket_x86] sys_socket,
    [SYS_bind] sys_bind,
    [SYS_bind_x86] sys_bind,
    [SYS_listen] sys_listen,
    [SYS_listen_x86] sys_listen,
    [SYS_accept] sys_accept,
    [SYS_accept_x86] sys_accept,
    [SYS_sconnect] sys_sconnect,
    [SYS_connect_x86] sys_sconnect,
    [SYS_sendto] sys_sendto,
    [SYS_sendto_x86] sys_sendto,
    [SYS_recvfrom] sys_recvfrom,
    [SYS_recvfrom_x86] sys_recvfrom,
    [SYS_setsockopt] sys_setsockopt,
    [SYS_setsockopt_x86] sys_setsockopt,
    [SYS_getsockopt] sys_getsockopt,
    [SYS_getsockopt_x86] sys_getsockopt,
    [SYS_shutdown] sys_shutdown,
    [SYS_shutdown_x86] sys_shutdown,
    [SYS_getpeername] sys_getpeername,
    [SYS_getpeername_x86] sys_getpeername,
    [SYS_getsockname] sys_getsockname,
    [SYS_getsockname_x86] sys_getsockname,
    [SYS_sendmsg] sys_sendmsg,
    [SYS_sendmsg_x86] sys_sendmsg,
    [SYS_recvmsg] sys_recvmsg,
    [SYS_recvmsg_x86] sys_recvmsg,
    [SYS_accept4] sys_accept4,
    [SYS_accept4_x86] sys_accept4,
    [SYS_sendfile] sys_sendfile,
    [SYS_sendfile_x86] sys_sendfile,
    [SYS_socketpair] sys_socketpair,
    [SYS_socketpair_x86] sys_socketpair,
    [SYS_sendmmsg] sys_sendmmsg,
    [SYS_sendmmsg_x86] sys_sendmmsg,
#endif
    [SYS_sched_rr_get_interval_time64] sys_sched_rr_get_interval,
    [SYS_sched_getaffinity] sys_sched_getaffinity,
    [SYS_sched_setaffinity] sys_sched_setaffinity,
    [SYS_sched_getaffinity_x86] sys_sched_getaffinity,
    [SYS_sched_setaffinity_x86] sys_sched_setaffinity,
    [SYS_sched_yield] sys_sched_yield,
    [SYS_sched_yield_x86] sys_sched_yield,
    [SYS_sched_getscheduler] sys_sched_getscheduler,
    [SYS_sched_getscheduler_x86] sys_sched_getscheduler,
    [SYS_sched_setscheduler] sys_sched_setscheduler,
    [SYS_sched_setscheduler_x86] sys_sched_setscheduler,
    [SYS_sched_getparam_x86] sys_sched_getparam,
    [SYS_sched_setparam_x86] sys_sched_setparam,
    [SYS_sched_getattr_x86] sys_sched_getattr,
    [SYS_sched_setattr_x86] sys_sched_setattr,
    [SYS_sched_get_priority_max] sys_sched_get_priority_max,
    [SYS_sched_get_priority_max_x86] sys_sched_get_priority_max,
    [SYS_sched_get_priority_min] sys_sched_get_priority_min,
    [SYS_sched_get_priority_min_x86] sys_sched_get_priority_min,
    [SYS_sched_rr_get_interval_x86] sys_sched_rr_get_interval,
    [SYS_ioprio_get_x86] sys_ioprio_get,
    [SYS_ioprio_set_x86] sys_ioprio_set,
#ifdef USE_LWIP
    [SYS_recvmmsg_time64] sys_recvmmsg,
    [SYS_recvmmsg_x86] sys_recvmmsg,
#else
    [SYS_recvmmsg_time64] sys_ni_enosys,
    [SYS_recvmmsg_x86] sys_ni_enosys,
#endif
    [SYS_pselect6_time64] sys_pselect6,
    [SYS_mq_timedsend_time64] sys_ni_enosys,
    [SYS_mq_timedreceive_time64] sys_ni_enosys,
    [SYS_signalfd] sys_signalfd,
    [SYS_signalfd4] sys_signalfd4,
    [SYS_eventfd2] sys_eventfd2,
    [SYS_eventfd_x86] sys_eventfd,
    [SYS_eventfd2_legacy] sys_eventfd2,
    [SYS_timerfd_create] sys_timerfd_create,
    [SYS_timerfd_create_x86] sys_timerfd_create,
    [SYS_timerfd_settime] sys_timerfd_settime,
    [SYS_timerfd_settime_x86] sys_timerfd_settime,
    [SYS_timerfd_gettime] sys_timerfd_gettime,
    [SYS_timerfd_gettime_x86] sys_timerfd_gettime,
    [SYS_clock_nanosleep] sys_clock_nanosleep,
    [SYS_clock_nanosleep_x86] sys_clock_nanosleep,
    [SYS_epoll_pwait] sys_epoll_pwait,
    [SYS_epoll_pwait_legacy] sys_epoll_pwait,
    [SYS_epoll_pwait2] sys_epoll_pwait2,
    [SYS_epoll_wait] sys_epoll_wait,
    [SYS_epoll_ctl] sys_epoll_ctl,
    [SYS_epoll_ctl_legacy] sys_epoll_ctl,
    [SYS_epoll_create1] sys_epoll_create1,
    [SYS_epoll_create1_legacy] sys_epoll_create1,
    [SYS_epoll_create] sys_epoll_create,
    [SYS_umask] sys_umask,
    [SYS_umask_x86] sys_umask,
    [SYS_fchownat] sys_vfs_fchownat,
    [SYS_fchownat_x86] sys_vfs_fchownat,
    [SYS_chown_x86] sys_vfs_chown,
    [SYS_lchown_x86] sys_vfs_lchown,
    [SYS_fchown] sys_vfs_fchown,
    [SYS_fchown_x86] sys_vfs_fchown,
    [SYS_fchmodat] sys_vfs_fchmodat,
    [SYS_fchmodat_x86] sys_vfs_fchmodat,
    [SYS_fchmodat2_x86] sys_vfs_fchmodat,
    [SYS_chmod_x86] sys_vfs_chmod,
    [SYS_fchmod] sys_vfs_fchmod,
    [SYS_fchmod_x86] sys_vfs_fchmod,
    [SYS_getitimer] sys_getitimer,
    [SYS_getitimer_x86] sys_getitimer,
    [SYS_getrlimit_x86] sys_getrlimit,
    [SYS_alarm_x86] sys_alarm,
    [SYS_setitimer] sys_setitimer,
    [SYS_setitimer_x86] sys_setitimer,
    [SYS_ppoll] sys_vfs_ppoll,
    [SYS_ppoll_x86] sys_vfs_ppoll,
    [SYS_pselect6_x86] sys_pselect6,
    /* System V IPC */
    [SYS_semtimedop] sys_semtimedop,
    [SYS_semtimedop_x86] sys_semtimedop,
    [SYS_shmget] sys_shmget,
    [SYS_shmget_x86] sys_shmget,
    [SYS_shmdt] sys_shmdt,
    [SYS_shmdt_x86] sys_shmdt,
    [SYS_shmctl] sys_shmctl,
    [SYS_shmctl_x86] sys_shmctl,
    [SYS_shmat] sys_shmat,
    [SYS_shmat_x86] sys_shmat,
    [SYS_semop] sys_semop,
    [SYS_semop_x86] sys_semop,
    [SYS_semget] sys_semget,
    [SYS_semget_x86] sys_semget,
    [SYS_semctl] sys_semctl,
    [SYS_semctl_x86] sys_semctl,
    [SYS_msgsnd] sys_msgsnd,
    [SYS_msgsnd_x86] sys_msgsnd,
    [SYS_msgrcv] sys_msgrcv,
    [SYS_msgrcv_x86] sys_msgrcv,
    [SYS_msgget] sys_msgget,
    [SYS_msgget_x86] sys_msgget,
    [SYS_msgctl] sys_msgctl,
    [SYS_msgctl_x86] sys_msgctl,
    /* Process / VFS / signal syscalls */
    [SYS_utimensat] sys_utimensat,
    [SYS_utimensat_x86] sys_utimensat,
    [SYS_utime_x86] sys_utime,
    [SYS_utimes_x86] sys_utimes,
    [SYS_clone3] sys_clone3,
    [SYS_clone3_x86] sys_clone3,
    [SYS_rt_sigqueueinfo] sys_rt_sigqueueinfo,
    [SYS_rt_sigqueueinfo_x86] sys_rt_sigqueueinfo,
    [SYS_clock_settime] sys_clock_settime,
    [SYS_fdatasync] sys_vfs_fdatasync,
    [SYS_fdatasync_x86] sys_vfs_fdatasync,
    [SYS_fsync] sys_vfs_fsync,
    [SYS_fsync_x86] sys_vfs_fsync,
    [SYS_readahead_x86] sys_vfs_readahead,
    [SYS_get_robust_list] sys_get_robust_list,
    [SYS_get_robust_list_x86] sys_get_robust_list,
    [SYS_set_robust_list] sys_set_robust_list,
    [SYS_set_robust_list_x86] sys_set_robust_list,
    [SYS_sigaltstack] sys_sigaltstack,
    [SYS_sigaltstack_x86] sys_sigaltstack,
    [SYS_prctl] sys_prctl,
    [SYS_prctl_x86] sys_prctl,
    [SYS_sysinfo] sys_sysinfo,
    [SYS_sysinfo_x86] sys_sysinfo,
    [SYS_getrusage] sys_getrusage,
    [SYS_getrusage_x86] sys_getrusage,
    [SYS_times_x86] sys_times,
    [SYS_getcpu_x86] sys_getcpu,
    [SYS_rseq_x86] sys_rseq,
    [SYS_getpriority] sys_getpriority,
    [SYS_getpriority_x86] sys_getpriority,
    [SYS_setpriority] sys_setpriority,
    [SYS_setpriority_x86] sys_setpriority,
    [SYS_munlockall_x86] sys_munlockall,
    [SYS_munlock_x86] sys_munlock,
    [SYS_mlockall_x86] sys_mlockall,
    [SYS_mlock_x86] sys_mlock,
    [SYS_munlockall] sys_munlockall,
    [SYS_munlock] sys_munlock,
    [SYS_mlockall] sys_mlockall,
    [SYS_mlock2] sys_mlock2,
    [SYS_mlock2_x86] sys_mlock2,
    [SYS_memfd_create] sys_memfd_create,
    [SYS_statx] sys_statx,
    [SYS_statx_x86] sys_statx,
    [SYS_inotify_init_x86] sys_inotify_init,
    [SYS_inotify_init1] sys_inotify_init1,
    [SYS_inotify_init1_x86] sys_inotify_init1,
    [SYS_inotify_add_watch] sys_inotify_add_watch,
    [SYS_inotify_add_watch_x86] sys_inotify_add_watch,
    [SYS_inotify_rm_watch] sys_inotify_rm_watch,
    [SYS_inotify_rm_watch_x86] sys_inotify_rm_watch,
    /* Linux x86_64 native __NR_getrandom — OpenSSL direct call */
    [SYS_getrandom_x86] sys_getrandom,
    [SYS_memfd_create_x86] sys_memfd_create,
    [SYS_membarrier] sys_membarrier,
    [SYS_membarrier_x86] sys_membarrier,
    [SYS_fadvise64] sys_fadvise64,
    [SYS_fadvise64_x86] sys_fadvise64,
    [SYS_sync_file_range_x86] sys_vfs_sync_file_range,
    [SYS_syncfs_x86] sys_vfs_syncfs,
    [SYS_fallocate] sys_fallocate,
    [SYS_fallocate_x86] sys_fallocate,
    [SYS_futimesat_x86] sys_futimesat,
    [SYS_copy_file_range_x86] sys_vfs_copy_file_range,
    [SYS_setxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_lsetxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_fsetxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_getxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_lgetxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_fgetxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_listxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_llistxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_flistxattr_x86] sys_vfs_xattr_not_supported,
    [SYS_removexattr_x86] sys_vfs_xattr_not_supported,
    [SYS_lremovexattr_x86] sys_vfs_xattr_not_supported,
    [SYS_fremovexattr_x86] sys_vfs_xattr_not_supported,
    [SYS_poweroff] sys_poweroff,
    [SYS_reboot] sys_reboot,
    [SYS_reboot_x86] sys_reboot,
    /* Linux optional/privileged compatibility syscalls */
    [SYS_ptrace_x86] sys_ptrace,
    [SYS_syslog_x86] sys_syslog,
    [SYS_uselib_x86] sys_uselib,
    [SYS_personality_x86] sys_personality,
    [SYS_ustat_x86] sys_ustat,
    [SYS_sysfs_x86] sys_sysfs,
    [SYS_vhangup_x86] sys_vhangup,
    [SYS_modify_ldt_x86] sys_modify_ldt,
    [SYS_pivot_root_x86] sys_pivot_root,
    [SYS__sysctl_x86] sys__sysctl,
    [SYS_adjtimex_x86] sys_adjtimex,
    [SYS_acct_x86] sys_acct,
    [SYS_settimeofday_x86] sys_settimeofday,
    [SYS_swapon_x86] sys_swapon,
    [SYS_swapoff_x86] sys_swapoff,
    [SYS_sethostname_x86] sys_sethostname,
    [SYS_setdomainname_x86] sys_setdomainname,
    [SYS_iopl_x86] sys_iopl,
    [SYS_ioperm_x86] sys_ioperm,
    [SYS_create_module_x86] sys_create_module,
    [SYS_init_module_x86] sys_init_module,
    [SYS_delete_module_x86] sys_delete_module,
    [SYS_get_kernel_syms_x86] sys_get_kernel_syms,
    [SYS_query_module_x86] sys_query_module,
    [SYS_quotactl_x86] sys_quotactl,
    [SYS_nfsservctl_x86] sys_nfsservctl,
    [SYS_getpmsg_x86] sys_getpmsg,
    [SYS_putpmsg_x86] sys_putpmsg,
    [SYS_afs_syscall_x86] sys_afs_syscall,
    [SYS_tuxcall_x86] sys_tuxcall,
    [SYS_security_x86] sys_security,
    [SYS_set_thread_area_x86] sys_set_thread_area,
    [SYS_io_setup_x86] sys_io_setup,
    [SYS_io_destroy_x86] sys_io_destroy,
    [SYS_io_getevents_x86] sys_io_getevents,
    [SYS_io_submit_x86] sys_io_submit,
    [SYS_io_cancel_x86] sys_io_cancel,
    [SYS_get_thread_area_x86] sys_get_thread_area,
    [SYS_lookup_dcookie_x86] sys_lookup_dcookie,
    [SYS_epoll_ctl_old_x86] sys_epoll_ctl_old,
    [SYS_epoll_wait_old_x86] sys_epoll_wait_old,
    [SYS_remap_file_pages_x86] sys_remap_file_pages,
    [SYS_restart_syscall_x86] sys_restart_syscall,
    [SYS_timer_create_x86] sys_timer_create,
    [SYS_timer_settime_x86] sys_timer_settime,
    [SYS_timer_gettime_x86] sys_timer_gettime,
    [SYS_timer_getoverrun_x86] sys_timer_getoverrun,
    [SYS_timer_delete_x86] sys_timer_delete,
    [SYS_vserver_x86] sys_vserver,
    [SYS_mbind_x86] sys_mbind,
    [SYS_set_mempolicy_x86] sys_set_mempolicy,
    [SYS_get_mempolicy_x86] sys_get_mempolicy,
    [SYS_mq_open_x86] sys_mq_open,
    [SYS_mq_unlink_x86] sys_mq_unlink,
    [SYS_mq_timedsend_x86] sys_mq_timedsend,
    [SYS_mq_timedreceive_x86] sys_mq_timedreceive,
    [SYS_mq_notify_x86] sys_mq_notify,
    [SYS_mq_getsetattr_x86] sys_mq_getsetattr,
    [SYS_kexec_load_x86] sys_kexec_load,
    [SYS_add_key_x86] sys_add_key,
    [SYS_request_key_x86] sys_request_key,
    [SYS_keyctl_x86] sys_keyctl,
    [SYS_migrate_pages_x86] sys_migrate_pages,
    [SYS_unshare_x86] sys_unshare,
    [SYS_splice_x86] sys_splice,
    [SYS_tee_x86] sys_tee,
    [SYS_vmsplice_x86] sys_vmsplice,
    [SYS_move_pages_x86] sys_move_pages,
    [SYS_rt_tgsigqueueinfo_x86] sys_rt_tgsigqueueinfo,
    [SYS_perf_event_open_x86] sys_perf_event_open,
    [SYS_fanotify_init_x86] sys_fanotify_init,
    [SYS_fanotify_mark_x86] sys_fanotify_mark,
    [SYS_name_to_handle_at_x86] sys_name_to_handle_at,
    [SYS_open_by_handle_at_x86] sys_open_by_handle_at,
    [SYS_clock_adjtime_x86] sys_clock_adjtime,
    [SYS_setns_x86] sys_setns,
    [SYS_process_vm_readv_x86] sys_process_vm_readv,
    [SYS_process_vm_writev_x86] sys_process_vm_writev,
    [SYS_kcmp_x86] sys_kcmp,
    [SYS_finit_module_x86] sys_finit_module,
    [SYS_seccomp_x86] sys_seccomp,
    [SYS_kexec_file_load_x86] sys_kexec_file_load,
    [SYS_bpf_x86] sys_bpf,
    [SYS_execveat_x86] sys_execveat,
    [SYS_userfaultfd_x86] sys_userfaultfd,
    [SYS_pkey_mprotect_x86] sys_pkey_mprotect,
    [SYS_pkey_alloc_x86] sys_pkey_alloc,
    [SYS_pkey_free_x86] sys_pkey_free,
    [SYS_io_pgetevents_x86] sys_io_pgetevents,
    [SYS_pidfd_send_signal_x86] sys_pidfd_send_signal,
    [SYS_io_uring_setup_x86] sys_io_uring_setup,
    [SYS_io_uring_enter_x86] sys_io_uring_enter,
    [SYS_io_uring_register_x86] sys_io_uring_register,
    [SYS_open_tree_x86] sys_open_tree,
    [SYS_move_mount_x86] sys_move_mount,
    [SYS_fsopen_x86] sys_fsopen,
    [SYS_fsconfig_x86] sys_fsconfig,
    [SYS_fsmount_x86] sys_fsmount,
    [SYS_fspick_x86] sys_fspick,
    [SYS_pidfd_open_x86] sys_pidfd_open,
    [SYS_pidfd_getfd_x86] sys_pidfd_getfd,
    [SYS_process_madvise_x86] sys_process_madvise,
    [SYS_mount_setattr_x86] sys_mount_setattr,
    [SYS_quotactl_fd_x86] sys_quotactl_fd,
    [SYS_landlock_create_ruleset_x86] sys_landlock_create_ruleset,
    [SYS_landlock_add_rule_x86] sys_landlock_add_rule,
    [SYS_landlock_restrict_self_x86] sys_landlock_restrict_self,
    [SYS_memfd_secret_x86] sys_memfd_secret,
    [SYS_process_mrelease_x86] sys_process_mrelease,
    [SYS_futex_waitv_x86] sys_futex_waitv,
    [SYS_set_mempolicy_home_node_x86] sys_set_mempolicy_home_node,
    [SYS_cachestat_x86] sys_cachestat,
    [SYS_map_shadow_stack_x86] sys_map_shadow_stack,
    [SYS_futex_wake_x86] sys_futex_wake,
    [SYS_futex_wait_x86] sys_futex_wait,
    [SYS_futex_requeue_x86] sys_futex_requeue,
    [SYS_statmount_x86] sys_statmount,
    [SYS_listmount_x86] sys_listmount,
    [SYS_lsm_get_self_attr_x86] sys_lsm_get_self_attr,
    [SYS_lsm_set_self_attr_x86] sys_lsm_set_self_attr,
    [SYS_lsm_list_modules_x86] sys_lsm_list_modules,
    /* Resource limit syscalls (musl high numbers → prlimit64) */
    [SYS_getrlimit] sys_getrlimit,
    [SYS_setrlimit] sys_setrlimit,
    [SYS_setrlimit_x86] sys_setrlimit,
    [SYS_prlimit64_musl] sys_prlimit64,
};

#ifdef ENABLE_LEGACY_XV6_SYSCALL_ALIAS
static int legacy_xv6_syscall_alias(int num)
{
    switch (num) {
    case 68: return SYS_brk;
    case 69: return SYS_futex;
    case 70: return SYS_sigaction;
    case 71: return SYS_sigreturn;
    case 72: return SYS_sigpending;
    case 73: return SYS_sigprocmask;
    case 75: return SYS_sigsuspend;
    case 76: return SYS_sigwait;
    case 100: return SYS_socket;
    case 101: return SYS_bind;
    case 102: return SYS_listen;
    case 103: return SYS_accept;
    case 104: return SYS_sconnect;
    case 105: return SYS_sendto;
    case 106: return SYS_recvfrom;
    case 107: return SYS_setsockopt;
    case 108: return SYS_getsockopt;
    case 109: return SYS_shutdown;
    case 110: return SYS_getpeername;
    case 111: return SYS_getsockname;
    case 112: return SYS_sendmsg;
    case 113: return SYS_recvmsg;
    case 114: return SYS_accept4;
    case 115: return SYS_sendfile;
    case 120: return SYS_openat;
    case 121: return SYS_writev;
    case 122: return SYS_readv;
    case 123: return SYS_set_tid_address;
    case 124: return SYS_clock_gettime;
    case 125: return SYS_clock_getres;
    case 126: return SYS_pread64;
    case 127: return SYS_pwrite64;
    case 128: return SYS_fstatat;
    case 129: return SYS_pipe2;
    case 130: return SYS_mkdirat;
    case 131: return SYS_mknodat;
    case 132: return SYS_unlinkat;
    case 133: return SYS_linkat;
    case 134: return SYS_symlinkat;
    case 135: return SYS_readlinkat;
    case 136: return SYS_renameat;
    case 137: return SYS_faccessat;
    case 138: return SYS_dup3;
    case 139: return SYS_getuid;
    case 140: return SYS_geteuid;
    case 141: return SYS_getgid;
    case 142: return SYS_getegid;
    case 143: return SYS_setuid;
    case 144: return SYS_setgid;
    case 145: return SYS_setreuid;
    case 146: return SYS_setregid;
    case 147: return SYS_getgroups;
    case 148: return SYS_setgroups;
    default: return 0;
    }
}
#endif

static int looks_like_linux_munmap(uint64 addr, uint64 length)
{
    return addr >= UVMBOTTOM && addr < UVMTOP &&
           (addr & (PGSIZE - 1)) == 0 &&
           length != 0 && length < (UVMTOP - UVMBOTTOM);
}

static int chrome_trace_parse_uint(const char *value, int default_value,
                                   int max_value)
{
    int parsed = 0;
    int saw_digit = 0;

    if (value == NULL)
        return default_value;
    while (*value >= '0' && *value <= '9') {
        saw_digit = 1;
        if (parsed < max_value)
            parsed = parsed * 10 + (*value - '0');
        if (parsed > max_value)
            parsed = max_value;
        value++;
    }
    return saw_digit ? parsed : default_value;
}

static int chrome_syscall_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_syscall_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_syscall_trace_limit(void)
{
    static int initialized;
    static int limit = 4096;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_syscall_trace_limit", value,
                              sizeof(value)) == 0) {
            limit = chrome_trace_parse_uint(value, 4096, 65536);
        }
        initialized = 1;
    }
    return limit;
}

static int chrome_syscall_enter_trace_limit(void)
{
    static int initialized;
    static int limit = 2048;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_syscall_enter_trace_limit", value,
                              sizeof(value)) == 0) {
            limit = chrome_trace_parse_uint(value, 2048, 65536);
        }
        initialized = 1;
    }
    return limit;
}

static int chrome_syscall_enter_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_syscall_enter_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_ppoll_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_ppoll_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_ppoll_trace_limit(void)
{
    static int initialized;
    static int limit = 128;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_ppoll_trace_limit", value,
                              sizeof(value)) == 0) {
            limit = chrome_trace_parse_uint(value, 128, 8192);
        }
        initialized = 1;
    }
    return limit;
}

static int chrome_syscall_enter_ipc_only_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_syscall_enter_ipc_only", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_syscall_enter_read_trace_enabled(void)
{
    static int initialized;
    static int enabled = 1;
    char value[8];

    if (!initialized) {
        if (cmdline_get_param("chrome_syscall_enter_read_trace", value,
                              sizeof(value)) == 0 &&
            (value[0] == '0' || value[0] == 'n' || value[0] == 'N')) {
            enabled = 0;
        }
        initialized = 1;
    }
    return enabled;
}

static int chrome_syscall_enter_futex_wake_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_syscall_enter_futex_wake_trace",
                                    value, sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_syscall_slow_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_syscall_slow_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static uint64 chrome_syscall_slow_trace_threshold_us(void)
{
    static int initialized;
    static uint64 threshold_us = 10000;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_syscall_slow_trace_us", value,
                              sizeof(value)) == 0) {
            threshold_us = chrome_trace_parse_uint(value, 10000, 10000000);
        }
        initialized = 1;
    }
    return threshold_us;
}

static int chrome_syscall_slow_trace_limit(void)
{
    static int initialized;
    static int limit = 512;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_syscall_slow_trace_limit", value,
                              sizeof(value)) == 0) {
            limit = chrome_trace_parse_uint(value, 512, 16384);
        }
        initialized = 1;
    }
    return limit;
}

static int chrome_syscall_progress_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_syscall_progress_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_syscall_progress_interval(void)
{
    static int initialized;
    static int interval = 2048;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_syscall_progress_interval", value,
                              sizeof(value)) == 0) {
            interval = chrome_trace_parse_uint(value, 2048, 1048576);
        }
        if (interval < 1)
            interval = 1;
        initialized = 1;
    }
    return interval;
}

static int chrome_syscall_progress_limit(void)
{
    static int initialized;
    static int limit = 512;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_syscall_progress_limit", value,
                              sizeof(value)) == 0) {
            limit = chrome_trace_parse_uint(value, 512, 16384);
        }
        initialized = 1;
    }
    return limit;
}

static int chrome_syscall_tail_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_syscall_tail_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_syscall_trace_process(struct thread *p);

struct chrome_thread_dump_ctx {
    int sample;
    int count;
};

static const char *chrome_thread_vma_path(vma_t *vma)
{
    if (vma == NULL || vma->file == NULL)
        return "-";
    if (vma->file->opened_path != NULL &&
        vma->file->opened_path[0] != '\0')
        return vma->file->opened_path;
    if (vma->file->inode.inode != NULL &&
        vma->file->inode.inode->name != NULL)
        return vma->file->inode.inode->name;
    return "(unnamed)";
}

static int chrome_thread_vma_interesting(vma_t *vma, uint64 rip, uint64 rsp)
{
    if (vma == NULL)
        return 0;
    if (rip >= vma->start && rip < vma->end)
        return 1;
    if (rsp >= vma->start && rsp < vma->end)
        return 1;
    return 0;
}

static void chrome_thread_dump_vmas(struct thread *t, uint64 rip, uint64 rsp)
{
    if (t == NULL || t->vm == NULL)
        return;

    vm_rlock(t->vm);
    vma_t *vma;
    uint64 index = 0;
    int found = 0;
    mt_for_each(&t->vm->vm_mt, vma, index, (uint64)(-1ULL)) {
        if (!chrome_thread_vma_interesting(vma, rip, rsp))
            continue;
        found++;
        printf("chrome-thread-vma: pid=%d tgid=%d name=%s "
               "map=[0x%lx-0x%lx) %c%c%c %s flags=0x%lx pgoff=0x%lx "
               "file=%p path=%s rip_in=%d rsp_in=%d\n",
               t->pid, t->tgid, t->name, vma->start, vma->end,
               (vma->flags & PROT_READ) ? 'r' : '-',
               (vma->flags & PROT_WRITE) ? 'w' : '-',
               (vma->flags & PROT_EXEC) ? 'x' : '-',
               (vma->flags & VMA_FLAG_SHARED) ? "shared" : "private",
               vma->flags, vma->pgoff, (void *)vma->file,
               chrome_thread_vma_path(vma),
               rip >= vma->start && rip < vma->end,
               rsp >= vma->start && rsp < vma->end);
    }
    vm_runlock(t->vm);

    if (found == 0)
        printf("chrome-thread-vma: pid=%d tgid=%d name=%s found=0 "
               "rip=0x%lx rsp=0x%lx\n",
               t->pid, t->tgid, t->name, rip, rsp);
}

static int chrome_thread_dump_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_thread_dump", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int chrome_thread_dump_samples(void)
{
    static int initialized;
    static int samples = 6;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_thread_dump_samples", value,
                              sizeof(value)) == 0) {
            samples = chrome_trace_parse_uint(value, 6, 64);
        }
        if (samples < 1)
            samples = 1;
        initialized = 1;
    }
    return samples;
}

static int chrome_thread_dump_interval_ms(void)
{
    static int initialized;
    static int interval_ms = 15000;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("chrome_thread_dump_interval_ms", value,
                              sizeof(value)) == 0) {
            interval_ms = chrome_trace_parse_uint(value, 15000, 120000);
        }
        if (interval_ms < 100)
            interval_ms = 100;
        initialized = 1;
    }
    return interval_ms;
}

static void chrome_thread_dump_cb(struct thread *t, void *arg)
{
    struct chrome_thread_dump_ctx *ctx = arg;

    if (t == NULL || ctx == NULL || !chrome_syscall_trace_process(t))
        return;

    enum thread_state state = __thread_state_get(t);
    int cpu = t->sched_entity ? t->sched_entity->cpu_id : -1;
    int on_cpu = t->sched_entity ?
        smp_load_acquire(&t->sched_entity->on_cpu) : 0;
    void *chan = t->chan;
    char name[sizeof(t->name)];
    const char *exec_path = "";
    uint64 rip = 0, rsp = 0, rax = 0, rdi = 0, rsi = 0, rdx = 0;

    if (t->trapframe != NULL) {
        rip = t->trapframe->trapframe.rip;
        rsp = t->trapframe->trapframe.rsp;
        rax = t->trapframe->trapframe.rax;
        rdi = t->trapframe->trapframe.rdi;
        rsi = t->trapframe->trapframe.rsi;
        rdx = t->trapframe->trapframe.rdx;
    }

    safestrcpy(name, t->name, sizeof(name));
    if (t->thread_group != NULL && t->thread_group->exec_path[0] != '\0')
        exec_path = t->thread_group->exec_path;
    ctx->count++;
    printf("chrome-thread-dump: sample=%d pid=%d tgid=%d name=%s "
           "state=%s cpu=%d on_cpu=%d chan=%p rip=0x%lx rsp=0x%lx "
           "rax=0x%lx rdi=0x%lx rsi=0x%lx rdx=0x%lx exec=%s\n",
           ctx->sample, t->pid, t->tgid, name, thread_state_short(state),
           cpu, on_cpu, chan, rip, rsp, rax, rdi, rsi, rdx, exec_path);
    if (ctx->sample == 1 &&
        (strncmp(name, "QDBusConnection", 15) == 0 ||
         strncmp(name, "kwin_wayland", 12) == 0)) {
        vfs_fdtable_debug_dump(t, "thread-dump", 16);
        chrome_thread_dump_vmas(t, rip, rsp);
    }
}

static void chrome_thread_dump_worker(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;

    for (int sample = 0; sample < chrome_thread_dump_samples(); sample++) {
        sleep_ms(sample == 0 ? 3000 : chrome_thread_dump_interval_ms());
        struct chrome_thread_dump_ctx ctx = {
            .sample = sample,
        };
        printf("chrome-thread-dump: begin sample=%d\n", sample);
        proctab_for_each_rcu(chrome_thread_dump_cb, &ctx);
        printf("chrome-thread-dump: end sample=%d count=%d\n",
               sample, ctx.count);
    }
}

static void maybe_start_chrome_thread_dump(struct thread *p)
{
    static _Atomic int started;

    if (!chrome_thread_dump_enabled() || !chrome_syscall_trace_process(p))
        return;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL) != 0)
        return;

    struct thread *t = kthread_create("chrome_thr_dump",
                                      chrome_thread_dump_worker,
                                      0, 0, KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("chrome-thread-dump: failed to start\n");
        return;
    }
    wakeup(t);
}

static int chrome_syscall_trace_process(struct thread *p)
{
    static int initialized;
    static int network_service_only;
    static int audio_service_only;
    static int child_processes;
    static int crashpad_processes;
    static char process_name[32];
    char value[8];
    char name_value[sizeof(process_name)];

    if (!initialized) {
        network_service_only =
            cmdline_get_param("chrome_syscall_trace_network_service", value,
                              sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        audio_service_only =
            cmdline_get_param("chrome_syscall_trace_audio_service", value,
                              sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        child_processes =
            cmdline_get_param("chrome_syscall_trace_child_processes", value,
                              sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        crashpad_processes =
            cmdline_get_param("chrome_syscall_trace_crashpad", value,
                              sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        if (cmdline_get_param("syscall_trace_name", name_value,
                              sizeof(name_value)) == 0 &&
            name_value[0] != '\0') {
            safestrcpy(process_name, name_value, sizeof(process_name));
        }
        initialized = 1;
    }
    if (p == NULL)
        return 0;
    if (process_name[0] != '\0') {
        const char *exec_path = "";
        const char *base;

        if (strncmp(p->name, process_name, sizeof(p->name)) == 0)
            return 1;
        if (p->thread_group != NULL &&
            p->thread_group->exec_path[0] != '\0')
            exec_path = p->thread_group->exec_path;
        base = strrchr(exec_path, '/');
        base = base ? base + 1 : exec_path;
        if (strcmp(base, process_name) == 0)
            return 1;
    }
    int is_crashpad = strncmp(p->name, "chrome_crashpad", 15) == 0;
    if (is_crashpad)
        return crashpad_processes;
    if (network_service_only || audio_service_only || child_processes)
        return (network_service_only &&
                chrome_lifecycle_network_service_match(p)) ||
               (audio_service_only &&
                chrome_lifecycle_audio_service_match(p)) ||
               (child_processes &&
                chrome_lifecycle_child_process_match(p));
    return chrome_lifecycle_thread_match(p);
}

#define TRACE_FUTEX_WAKE        1
#define TRACE_FUTEX_WAKE_OP     5
#define TRACE_FUTEX_WAKE_BITSET 10
#define TRACE_FUTEX_PRIVATE     128
#define TRACE_FUTEX_REALTIME    256

static int chrome_syscall_trace_interesting(int orig_num, uint64 a1)
{
    switch (orig_num) {
    case SYS_read_x86:
        return chrome_syscall_enter_read_trace_enabled();
    case SYS_write_x86:
    case SYS_close_x86:
    case SYS_mmap_x86:
    case SYS_mprotect_x86:
    case SYS_munmap_x86:
    case SYS_brk_x86:
    case SYS_rt_sigaction_x86:
    case SYS_rt_sigprocmask_x86:
    case SYS_getpid_x86:
    case SYS_getuid_x86:
    case SYS_getgid_x86:
    case SYS_geteuid_x86:
    case SYS_getegid_x86:
    case SYS_getppid_x86:
    case SYS_gettid_x86:
    case SYS_clock_gettime_x86:
    case SYS_clock_getres_x86:
    case SYS_getrandom_x86:
        return 0;
    case SYS_futex_x86:
    {
        uint64 futex_cmd = a1 & ~(TRACE_FUTEX_PRIVATE | TRACE_FUTEX_REALTIME);
        if (futex_cmd == TRACE_FUTEX_WAKE ||
            futex_cmd == TRACE_FUTEX_WAKE_OP ||
            futex_cmd == TRACE_FUTEX_WAKE_BITSET) {
            return chrome_syscall_enter_futex_wake_trace_enabled();
        }
        return 1;
    }
    default:
        return 1;
    }
}

static int chrome_syscall_enter_trace_interesting(int orig_num, uint64 a1)
{
    int ipc_only = chrome_syscall_enter_ipc_only_enabled();

    switch (orig_num) {
    case SYS_read_x86:
        return !ipc_only && chrome_syscall_enter_read_trace_enabled();
    case SYS_poll_x86:
    case SYS_select_x86:
    case SYS_connect_x86:
    case SYS_recvfrom_x86:
    case SYS_sendmsg_x86:
    case SYS_recvmsg_x86:
    case SYS_accept4_x86:
        return 1;
    case SYS_openat_x86:
        return !ipc_only;
    case SYS_mmap_x86:
    case SYS_madvise_x86:
    case SYS_mprotect_x86:
    case SYS_rt_sigprocmask_x86:
    case SYS_sched_getaffinity_x86:
    case SYS_prlimit64_x86:
    case SYS_geteuid_x86:
    case SYS_getresuid_x86:
    case SYS_getrandom_x86:
    case SYS_rseq_x86:
        return !ipc_only;
    case SYS_clone3_x86:
    case SYS_prctl_x86:
    case SYS_unshare_x86:
    case SYS_setns_x86:
    case SYS_seccomp_x86:
        return 1;
    case SYS_futex_x86:
    {
        uint64 futex_cmd = a1 & ~(TRACE_FUTEX_PRIVATE | TRACE_FUTEX_REALTIME);
        if (futex_cmd == TRACE_FUTEX_WAKE ||
            futex_cmd == TRACE_FUTEX_WAKE_OP ||
            futex_cmd == TRACE_FUTEX_WAKE_BITSET) {
            return chrome_syscall_enter_futex_wake_trace_enabled();
        }
        return 1;
    }
    case SYS_wait4_x86:
    case SYS_waitid_x86:
    case SYS_nanosleep_x86:
    case SYS_pselect6_x86:
    case SYS_ppoll_x86:
    case SYS_epoll_pwait:
    case SYS_epoll_pwait_legacy:
    case SYS_epoll_pwait2:
    case SYS_epoll_wait:
    case SYS_clock_nanosleep_x86:
    case SYS_futex_wait_x86:
    case SYS_futex_waitv_x86:
        return 1;
    default:
        return 0;
    }
}

static const char *x86_syscall_trace_name(int num)
{
    switch (num) {
    case SYS_read_x86: return "read";
    case SYS_write_x86: return "write";
    case SYS_open_x86: return "open";
    case SYS_close_x86: return "close";
    case SYS_creat_x86: return "creat";
    case SYS_fstat_x86: return "fstat";
    case SYS_stat_x86: return "stat";
    case SYS_lstat_x86: return "lstat";
    case SYS_lseek_x86: return "lseek";
    case SYS_mmap_x86: return "mmap";
    case SYS_mprotect_x86: return "mprotect";
    case SYS_munmap_x86: return "munmap";
    case SYS_brk_x86: return "brk";
    case SYS_rt_sigaction_x86: return "rt_sigaction";
    case SYS_rt_sigprocmask_x86: return "rt_sigprocmask";
    case SYS_sigaltstack_x86: return "sigaltstack";
    case SYS_ioctl_x86: return "ioctl";
    case SYS_pread64_x86: return "pread64";
    case SYS_pwrite64_x86: return "pwrite64";
    case SYS_readv_x86: return "readv";
    case SYS_writev_x86: return "writev";
    case SYS_access_x86: return "access";
    case SYS_poll_x86: return "poll";
    case SYS_select_x86: return "select";
    case SYS_mremap_x86: return "mremap";
    case SYS_madvise_x86: return "madvise";
    case SYS_nanosleep_x86: return "nanosleep";
    case SYS_getpid_x86: return "getpid";
    case SYS_socket_x86: return "socket";
    case SYS_bind_x86: return "bind";
    case SYS_listen_x86: return "listen";
    case SYS_connect_x86: return "connect";
    case SYS_sendto_x86: return "sendto";
    case SYS_recvfrom_x86: return "recvfrom";
    case SYS_sendmsg_x86: return "sendmsg";
    case SYS_recvmsg_x86: return "recvmsg";
    case SYS_getpeername_x86: return "getpeername";
    case SYS_getsockname_x86: return "getsockname";
    case SYS_setsockopt_x86: return "setsockopt";
    case SYS_getsockopt_x86: return "getsockopt";
    case SYS_socketpair_x86: return "socketpair";
    case SYS_clone_x86: return "clone";
    case SYS_execve_x86: return "execve";
    case SYS_exit_x86: return "exit";
    case SYS_wait4_x86: return "wait4";
    case SYS_kill_x86: return "kill";
    case SYS_uname_x86: return "uname";
    case SYS_fcntl_x86: return "fcntl";
    case SYS_fsync_x86: return "fsync";
    case SYS_fdatasync_x86: return "fdatasync";
    case SYS_ftruncate_x86: return "ftruncate";
    case SYS_fchown_x86: return "fchown";
    case SYS_statfs_x86: return "statfs";
    case SYS_fstatfs_x86: return "fstatfs";
    case SYS_getcwd_x86: return "getcwd";
    case SYS_chdir_x86: return "chdir";
    case SYS_mkdir_x86: return "mkdir";
    case SYS_unlink_x86: return "unlink";
    case SYS_readlink_x86: return "readlink";
    case SYS_gettimeofday_x86: return "gettimeofday";
    case SYS_getrlimit_x86: return "getrlimit";
    case SYS_getuid_x86: return "getuid";
    case SYS_getgid_x86: return "getgid";
    case SYS_geteuid_x86: return "geteuid";
    case SYS_getegid_x86: return "getegid";
    case SYS_getppid_x86: return "getppid";
    case SYS_setsid_x86: return "setsid";
    case SYS_gettid_x86: return "gettid";
    case SYS_futex_x86: return "futex";
    case SYS_getpriority_x86: return "getpriority";
    case SYS_setpriority_x86: return "setpriority";
    case SYS_sched_getaffinity_x86: return "sched_getaffinity";
    case SYS_set_tid_address_x86: return "set_tid_address";
    case SYS_clock_gettime_x86: return "clock_gettime";
    case SYS_clock_getres_x86: return "clock_getres";
    case SYS_exit_group_x86: return "exit_group";
    case SYS_tgkill_x86: return "tgkill";
    case SYS_openat_x86: return "openat";
    case SYS_newfstatat_x86: return "newfstatat";
    case SYS_readlinkat_x86: return "readlinkat";
    case SYS_faccessat_x86: return "faccessat";
    case SYS_fchownat_x86: return "fchownat";
    case SYS_pselect6_x86: return "pselect6";
    case SYS_ppoll_x86: return "ppoll";
    case SYS_set_robust_list_x86: return "set_robust_list";
    case SYS_capget_x86: return "capget";
    case SYS_capset_x86: return "capset";
    case SYS_arch_prctl: return "arch_prctl";
    case SYS_epoll_create1: return "epoll_create1";
    case SYS_epoll_create1_legacy: return "epoll_create1";
    case SYS_epoll_create: return "epoll_create";
    case SYS_epoll_ctl: return "epoll_ctl";
    case SYS_eventfd2: return "eventfd2";
    case SYS_eventfd2_legacy: return "eventfd2";
    case SYS_timerfd_create_x86: return "timerfd_create";
    case SYS_timerfd_settime_x86: return "timerfd_settime";
    case SYS_timerfd_gettime_x86: return "timerfd_gettime";
    case SYS_eventfd_x86: return "eventfd";
    case SYS_pipe2_x86: return "pipe2";
    case SYS_inotify_add_watch_x86: return "inotify_add_watch";
    case SYS_fallocate_x86: return "fallocate";
    case SYS_dup2_x86: return "dup2";
    case SYS_accept4_x86: return "accept4";
    case SYS_dup3_x86: return "dup3";
    case SYS_memfd_create_x86: return "memfd_create";
    case SYS_prlimit64_x86: return "prlimit64";
    case SYS_getrandom_x86: return "getrandom";
    case SYS_membarrier_x86: return "membarrier";
    case SYS_statx_x86: return "statx";
    case SYS_rseq_x86: return "rseq";
    case SYS_prctl_x86: return "prctl";
    case SYS_sysinfo_x86: return "sysinfo";
    case SYS_getdents64_x86: return "getdents64";
    case SYS_pkey_alloc_x86: return "pkey_alloc";
    case SYS_pkey_free_x86: return "pkey_free";
    case SYS_pkey_mprotect_x86: return "pkey_mprotect";
    case SYS_epoll_pwait: return "epoll_pwait";
    case SYS_epoll_pwait_legacy: return "epoll_pwait";
    case SYS_epoll_pwait2: return "epoll_pwait2";
    case SYS_epoll_wait: return "epoll_wait";
    case SYS_waitid_x86: return "waitid";
    case SYS_clone3_x86: return "clone3";
    case SYS_close_range_x86: return "close_range";
    case SYS_unshare_x86: return "unshare";
    case SYS_setns_x86: return "setns";
    case SYS_seccomp_x86: return "seccomp";
    case SYS_landlock_create_ruleset_x86: return "landlock_create_ruleset";
    case SYS_landlock_add_rule_x86: return "landlock_add_rule";
    case SYS_landlock_restrict_self_x86: return "landlock_restrict_self";
    case SYS_futex_wake_x86: return "futex_wake";
    case SYS_futex_wait_x86: return "futex_wait";
    default: return "?";
    }
}

static void chrome_syscall_trace_line(const char *phase, struct thread *p,
                                      int orig_num, int num, uint64 a0,
                                      uint64 a1, uint64 a2, uint64 a3,
                                      uint64 a4, uint64 a5, uint64 ret,
                                      int have_ret)
{
    if (have_ret) {
        uint32 roles = p->thread_group ?
            __atomic_load_n(&p->thread_group->chrome_trace_roles,
                            __ATOMIC_RELAXED) : 0;
        printf("chrome-syscall-%s: pid=%d tgid=%d roles=0x%x name=%s "
               "orig=%d num=%d(%s) a0=0x%lx a1=0x%lx a2=0x%lx "
               "a3=0x%lx a4=0x%lx a5=0x%lx ret=0x%lx\n",
               phase, p->pid, p->tgid, roles, p->name, orig_num, num,
               x86_syscall_trace_name(orig_num), a0, a1, a2, a3, a4, a5,
               ret);
    } else {
        uint32 roles = p->thread_group ?
            __atomic_load_n(&p->thread_group->chrome_trace_roles,
                            __ATOMIC_RELAXED) : 0;
        printf("chrome-syscall-%s: pid=%d tgid=%d roles=0x%x name=%s "
               "orig=%d num=%d(%s) a0=0x%lx a1=0x%lx a2=0x%lx "
               "a3=0x%lx a4=0x%lx a5=0x%lx\n",
               phase, p->pid, p->tgid, roles, p->name, orig_num, num,
               x86_syscall_trace_name(orig_num), a0, a1, a2, a3, a4, a5);
    }
}

static void chrome_syscall_trace_path_arg(const char *label, uint64 addr)
{
    char path[160];

    if (addr == 0)
        return;
    if (fetchstr(addr, path, sizeof(path)) < 0) {
        printf("chrome-syscall-detail: %s=<fault 0x%lx>\n", label, addr);
        return;
    }
    printf("chrome-syscall-detail: %s=%s\n", label, path);
}

struct trace_sockaddr_un {
    uint16 family;
    char path[108];
};

struct trace_msghdr {
    uint64 msg_name;
    uint64 msg_namelen;
    uint64 msg_iov;
    uint64 msg_iovlen;
    uint64 msg_control;
    uint64 msg_controllen;
    uint32 msg_flags;
};

struct trace_linux_clone3_args {
    uint64 flags;
    uint64 pidfd;
    uint64 child_tid;
    uint64 parent_tid;
    uint64 exit_signal;
    uint64 stack;
    uint64 stack_size;
    uint64 tls;
    uint64 set_tid;
    uint64 set_tid_size;
    uint64 cgroup;
};

static void chrome_syscall_trace_sockaddr(const char *label, uint64 addr,
                                          uint64 len)
{
    struct trace_sockaddr_un sa;
    size_t copy_len = len < sizeof(sa) ? len : sizeof(sa);

    if (addr == 0 || copy_len < sizeof(sa.family))
        return;
    memset(&sa, 0, sizeof(sa));
    if (vm_copyin(current->vm, &sa, addr, copy_len) < 0) {
        printf("chrome-syscall-detail: %s=<fault 0x%lx len=%lu>\n",
               label, addr, len);
        return;
    }
    if (sa.family == 1) {
        sa.path[sizeof(sa.path) - 1] = '\0';
        printf("chrome-syscall-detail: %s=AF_UNIX path=%s len=%lu\n",
               label, sa.path, len);
    } else {
        printf("chrome-syscall-detail: %s=family=%u len=%lu\n",
               label, sa.family, len);
    }
}

static void chrome_syscall_trace_msghdr(const char *syscall, uint64 addr)
{
    struct trace_msghdr msg;

    if (addr == 0)
        return;
    memset(&msg, 0, sizeof(msg));
    if (vm_copyin(current->vm, &msg, addr, sizeof(msg)) < 0) {
        printf("chrome-syscall-detail: %s msghdr=<fault 0x%lx>\n",
               syscall, addr);
        return;
    }
    printf("chrome-syscall-detail: %s name=0x%lx namelen=%lu "
           "iov=0x%lx iovlen=%lu control=0x%lx controllen=%lu flags=0x%x\n",
           syscall, msg.msg_name, msg.msg_namelen, msg.msg_iov,
           msg.msg_iovlen, msg.msg_control, msg.msg_controllen,
           msg.msg_flags);
    chrome_syscall_trace_sockaddr("msg.name", msg.msg_name,
                                  msg.msg_namelen);
}

static void chrome_syscall_trace_clone3(uint64 addr, uint64 size)
{
    struct trace_linux_clone3_args args;
    uint64 copy_len = size;
    uint64 unsupported_flags;
    uint64 known_flags;

    if (addr == 0)
        return;
    memset(&args, 0, sizeof(args));
    if (copy_len > sizeof(args))
        copy_len = sizeof(args);
    if (copy_len != 0 && vm_copyin(current->vm, &args, addr, copy_len) < 0) {
        printf("chrome-syscall-detail: clone3 args=<fault 0x%lx size=%lu>\n",
               addr, size);
        return;
    }

    unsupported_flags =
        CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS |
        CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET |
        CLONE_INTO_CGROUP | CLONE_PID | CLONE_SYSTEM | CLONE_SIGSTOPPED;
    known_flags =
        0xffULL | CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
        CLONE_PIDFD | CLONE_PTRACE | CLONE_VFORK | CLONE_PARENT |
        CLONE_THREAD | CLONE_NEWNS | CLONE_SYSVSEM | CLONE_SETTLS |
        CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED |
        CLONE_UNTRACED | CLONE_CHILD_SETTID | CLONE_NEWCGROUP |
        CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
        CLONE_NEWNET | CLONE_IO | CLONE_CLEAR_SIGHAND |
        CLONE_INTO_CGROUP | CLONE_PID | CLONE_SYSTEM | CLONE_SIGSTOPPED;

    printf("chrome-syscall-detail: clone3 size=%lu copied=%lu flags=0x%lx "
           "unsupported=0x%lx unknown=0x%lx exit_signal=%lu pidfd=0x%lx "
           "parent_tid=0x%lx child_tid=0x%lx stack=0x%lx stack_size=0x%lx "
           "tls=0x%lx set_tid=0x%lx set_tid_size=%lu cgroup=0x%lx\n",
           size, copy_len, args.flags, args.flags & unsupported_flags,
           args.flags & ~known_flags, args.exit_signal, args.pidfd,
           args.parent_tid, args.child_tid, args.stack, args.stack_size,
           args.tls, args.set_tid, args.set_tid_size, args.cgroup);
}

static const char *chrome_prctl_option_name(uint64 option)
{
    switch (option) {
    case 3: return "PR_GET_DUMPABLE";
    case 4: return "PR_SET_DUMPABLE";
    case 15: return "PR_SET_NAME";
    case 16: return "PR_GET_NAME";
    case 21: return "PR_GET_SECCOMP";
    case 22: return "PR_SET_SECCOMP";
    case 23: return "PR_CAPBSET_READ";
    case 24: return "PR_CAPBSET_DROP";
    case 27: return "PR_GET_SECUREBITS";
    case 28: return "PR_SET_SECUREBITS";
    case 29: return "PR_SET_TIMERSLACK";
    case 30: return "PR_GET_TIMERSLACK";
    case 38: return "PR_SET_NO_NEW_PRIVS";
    case 39: return "PR_GET_NO_NEW_PRIVS";
    case 0x53564d41: return "PR_SET_VMA";
    default: return "?";
    }
}

static const char *chrome_seccomp_op_name(uint64 op)
{
    switch (op) {
    case 0: return "SECCOMP_SET_MODE_STRICT";
    case 1: return "SECCOMP_SET_MODE_FILTER";
    case 2: return "SECCOMP_GET_ACTION_AVAIL";
    case 3: return "SECCOMP_GET_NOTIF_SIZES";
    default: return "?";
    }
}

static void chrome_syscall_trace_namespace_flags(const char *syscall,
                                                 uint64 flags)
{
    uint64 namespace_flags =
        CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS |
        CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET;
    uint64 known_flags =
        namespace_flags | CLONE_VM | CLONE_FS | CLONE_FILES |
        CLONE_SIGHAND | CLONE_SYSVSEM | CLONE_THREAD | CLONE_IO |
        CLONE_NEWTIME;

    printf("chrome-syscall-detail: %s flags=0x%lx namespace=0x%lx "
           "unknown=0x%lx newns=%d newcg=%d newuts=%d newipc=%d "
           "newuser=%d newpid=%d newnet=%d newtime=%d\n",
           syscall, flags, flags & namespace_flags, flags & ~known_flags,
           (flags & CLONE_NEWNS) != 0, (flags & CLONE_NEWCGROUP) != 0,
           (flags & CLONE_NEWUTS) != 0, (flags & CLONE_NEWIPC) != 0,
           (flags & CLONE_NEWUSER) != 0, (flags & CLONE_NEWPID) != 0,
           (flags & CLONE_NEWNET) != 0, (flags & CLONE_NEWTIME) != 0);
}

static void chrome_syscall_trace_prctl(uint64 option, uint64 arg2,
                                       uint64 arg3)
{
    printf("chrome-syscall-detail: prctl option=%lu(%s) arg2=0x%lx "
           "arg3=0x%lx\n",
           option, chrome_prctl_option_name(option), arg2, arg3);
}

static void chrome_syscall_trace_seccomp(uint64 op, uint64 flags, uint64 args)
{
    printf("chrome-syscall-detail: seccomp op=%lu(%s) flags=0x%lx "
           "args=0x%lx\n",
           op, chrome_seccomp_op_name(op), flags, args);
}

static void chrome_syscall_trace_details(int orig_num, uint64 a0, uint64 a1,
                                         uint64 a2)
{
    switch (orig_num) {
    case SYS_open_x86:
    case SYS_stat_x86:
    case SYS_lstat_x86:
    case SYS_access_x86:
    case SYS_readlink_x86:
    case SYS_symlink_x86:
        chrome_syscall_trace_path_arg("path", a0);
        break;
    case SYS_openat_x86:
    case SYS_newfstatat_x86:
    case SYS_readlinkat_x86:
    case SYS_faccessat_x86:
    case SYS_faccessat2_x86:
    case SYS_symlinkat_x86:
        chrome_syscall_trace_path_arg("path", a1);
        break;
    case SYS_connect_x86:
        chrome_syscall_trace_sockaddr("connect.addr", a1, a2);
        break;
    case SYS_sendmsg_x86:
        chrome_syscall_trace_msghdr("sendmsg", a1);
        break;
    case SYS_recvmsg_x86:
        chrome_syscall_trace_msghdr("recvmsg", a1);
        break;
    case SYS_clone3_x86:
        chrome_syscall_trace_clone3(a0, a1);
        break;
    case SYS_prctl_x86:
        chrome_syscall_trace_prctl(a0, a1, a2);
        break;
    case SYS_unshare_x86:
        chrome_syscall_trace_namespace_flags("unshare", a0);
        break;
    case SYS_setns_x86:
        chrome_syscall_trace_namespace_flags("setns", a1);
        break;
    case SYS_seccomp_x86:
        chrome_syscall_trace_seccomp(a0, a1, a2);
        break;
    default:
        break;
    }
}

static const char *chrome_trace_file_kind_name(struct vfs_file *f)
{
    if (f == NULL)
        return "(null)";
    switch (f->f_kind) {
    case VFS_FILE_KIND_NONE: return "none";
    case VFS_FILE_KIND_INODE: return "inode";
    case VFS_FILE_KIND_CDEV: return "cdev";
    case VFS_FILE_KIND_BDEV: return "bdev";
    case VFS_FILE_KIND_PIPE: return "pipe";
    case VFS_FILE_KIND_LEGACY_SOCKET: return "socket";
    case VFS_FILE_KIND_CUSTOM: return "custom";
    default: return "?";
    }
}

static void chrome_syscall_trace_fd_arg(const char *label, uint64 fd_arg)
{
    if (current == NULL || current->fdtable == NULL)
        return;
    if (fd_arg >= NOFILE) {
        printf("chrome-syscall-detail: %s=%ld fdpath=<invalid>\n",
               label, (long)fd_arg);
        return;
    }

    int fd = (int)fd_arg;
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL) {
        printf("chrome-syscall-detail: %s=%d fdpath=<closed>\n", label, fd);
        return;
    }

    char anon_path[96];
    const char *path = f->opened_path != NULL ? f->opened_path : NULL;
    if (path == NULL && f->ops != NULL && f->ops->readlink != NULL) {
        ssize_t n = f->ops->readlink(f, anon_path, sizeof(anon_path) - 1);
        if (n > 0) {
            if ((size_t)n >= sizeof(anon_path))
                n = sizeof(anon_path) - 1;
            anon_path[n] = '\0';
            path = anon_path;
        }
    }
    if (path == NULL)
        path = chrome_trace_file_kind_name(f);
    printf("chrome-syscall-detail: %s=%d fdpath=%s fkind=%s flags=0x%x "
           "pos=%lld file=%p ref=%d\n",
           label, fd, path, chrome_trace_file_kind_name(f), f->f_flags,
           f->f_pos, f, f->ref_count);
    vfs_fput(f);
}

struct chrome_trace_pollfd {
    int fd;
    short events;
    short revents;
};

static void chrome_syscall_trace_pollfds(uint64 pfds_addr, uint64 nfds_arg)
{
    if (current == NULL || current->vm == NULL)
        return;
    if (pfds_addr == 0 || nfds_arg == 0)
        return;

    uint64 nfds = nfds_arg;
    if (nfds > 4)
        nfds = 4;

    struct chrome_trace_pollfd pfds[4];
    size_t bytes = (size_t)nfds * sizeof(pfds[0]);
    if (vm_copyin(current->vm, pfds, pfds_addr, bytes) < 0) {
        printf("chrome-syscall-detail: pollfds=0x%lx nfds=%ld copy=<fault>\n",
               pfds_addr, (long)nfds_arg);
        return;
    }

    printf("chrome-syscall-detail: pollfds=0x%lx nfds=%ld showing=%ld\n",
           pfds_addr, (long)nfds_arg, (long)nfds);
    for (uint64 i = 0; i < nfds; i++) {
        printf("chrome-syscall-detail: pollfd[%ld] fd=%d events=0x%x "
               "revents=0x%x\n",
               (long)i, pfds[i].fd, pfds[i].events, pfds[i].revents);
        if (pfds[i].fd >= 0)
            chrome_syscall_trace_fd_arg("pollfd", (uint64)pfds[i].fd);
    }
}

static uint64 chrome_trace_ticks_to_us(uint64 ticks)
{
    uint64 freq = __timebase_frequency ? __timebase_frequency : 10000000UL;

    return (ticks / freq) * 1000000ULL +
           ((ticks % freq) * 1000000ULL) / freq;
}

static void chrome_syscall_slow_trace_line(struct thread *p, int orig_num,
                                           int num, uint64 a0, uint64 a1,
                                           uint64 a2, uint64 a3, uint64 a4,
                                           uint64 a5, uint64 ret,
                                           uint64 elapsed_ticks)
{
    int cpu = -1;
    enum thread_state state = THREAD_UNUSED;
    void *chan = NULL;

    if (p->sched_entity != NULL)
        cpu = p->sched_entity->cpu_id;
    state = p->state;
    chan = p->chan;
    printf("chrome-syscall-slow: pid=%d tgid=%d name=%s cpu=%d "
           "state=%s chan=%p orig=%d num=%d(%s) us=%lu "
           "a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx a4=0x%lx "
           "a5=0x%lx ret=0x%lx\n",
           p->pid, p->tgid, p->name, cpu, thread_state_short(state), chan,
           orig_num, num, x86_syscall_trace_name(orig_num),
           chrome_trace_ticks_to_us(elapsed_ticks), a0, a1, a2, a3, a4, a5,
           ret);
}

#define CHROME_SYSCALL_PROGRESS_SLOTS 64

struct chrome_syscall_progress_slot {
    int pid;
    uint64 count;
};

static struct chrome_syscall_progress_slot chrome_progress_slots
    [CHROME_SYSCALL_PROGRESS_SLOTS];

static struct chrome_syscall_progress_slot *
chrome_syscall_progress_slot(struct thread *p)
{
    int empty = -1;

    if (p == NULL)
        return NULL;
    for (int i = 0; i < CHROME_SYSCALL_PROGRESS_SLOTS; i++) {
        int pid = __atomic_load_n(&chrome_progress_slots[i].pid,
                                  __ATOMIC_RELAXED);
        if (pid == p->pid)
            return &chrome_progress_slots[i];
        if (pid == 0 && empty < 0)
            empty = i;
    }
    if (empty >= 0) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&chrome_progress_slots[empty].pid,
                                        &expected, p->pid, 0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return &chrome_progress_slots[empty];
    }
    return NULL;
}

static void chrome_syscall_progress_trace_line(struct thread *p, int orig_num,
                                               int num, uint64 a0, uint64 a1,
                                               uint64 a2, uint64 ret,
                                               uint64 count)
{
    int cpu = -1;

    if (p->sched_entity != NULL)
        cpu = p->sched_entity->cpu_id;
    printf("chrome-syscall-progress: pid=%d tgid=%d name=%s cpu=%d "
           "state=%s count=%lu orig=%d num=%d(%s) a0=0x%lx a1=0x%lx "
           "a2=0x%lx ret=0x%lx\n",
           p->pid, p->tgid, p->name, cpu, thread_state_short(p->state),
           count, orig_num, num, x86_syscall_trace_name(orig_num), a0, a1,
           a2, ret);
}

#define CHROME_SYSCALL_TAIL_SLOTS 64
#define CHROME_SYSCALL_TAIL_DEPTH 64

enum chrome_syscall_tail_family {
    CHROME_SYSCALL_TAIL_OTHER = 0,
    CHROME_SYSCALL_TAIL_CLONE,
    CHROME_SYSCALL_TAIL_WAIT,
    CHROME_SYSCALL_TAIL_FUTEX,
    CHROME_SYSCALL_TAIL_EPOLL,
    CHROME_SYSCALL_TAIL_POLL,
    CHROME_SYSCALL_TAIL_OPEN,
    CHROME_SYSCALL_TAIL_FCNTL,
    CHROME_SYSCALL_TAIL_IOCTL,
    CHROME_SYSCALL_TAIL_MMAP,
    CHROME_SYSCALL_TAIL_SCHED,
    CHROME_SYSCALL_TAIL_SIGNAL,
    CHROME_SYSCALL_TAIL_IPC,
    CHROME_SYSCALL_TAIL_MAX,
};

struct chrome_syscall_tail_entry {
    uint64 seq;
    uint64 ticks;
    uint64 elapsed_us;
    uint64 a0;
    uint64 a1;
    uint64 a2;
    uint64 a3;
    uint64 a4;
    uint64 a5;
    uint64 ret;
    int orig_num;
    int num;
    int pid;
    int tgid;
    int cpu;
    enum chrome_syscall_tail_family family;
};

struct chrome_syscall_tail_slot {
    int pid;
    uint64 next_seq;
    struct chrome_syscall_tail_entry entries[CHROME_SYSCALL_TAIL_DEPTH];
};

static struct chrome_syscall_tail_slot chrome_tail_slots
    [CHROME_SYSCALL_TAIL_SLOTS];

static const char *chrome_syscall_tail_family_name(
    enum chrome_syscall_tail_family family)
{
    switch (family) {
    case CHROME_SYSCALL_TAIL_CLONE: return "clone";
    case CHROME_SYSCALL_TAIL_WAIT: return "wait";
    case CHROME_SYSCALL_TAIL_FUTEX: return "futex";
    case CHROME_SYSCALL_TAIL_EPOLL: return "epoll";
    case CHROME_SYSCALL_TAIL_POLL: return "poll";
    case CHROME_SYSCALL_TAIL_OPEN: return "open";
    case CHROME_SYSCALL_TAIL_FCNTL: return "fcntl";
    case CHROME_SYSCALL_TAIL_IOCTL: return "ioctl";
    case CHROME_SYSCALL_TAIL_MMAP: return "mmap";
    case CHROME_SYSCALL_TAIL_SCHED: return "sched";
    case CHROME_SYSCALL_TAIL_SIGNAL: return "signal";
    case CHROME_SYSCALL_TAIL_IPC: return "ipc";
    case CHROME_SYSCALL_TAIL_OTHER:
    default:
        return "other";
    }
}

static enum chrome_syscall_tail_family
chrome_syscall_tail_family_for(int orig_num)
{
    switch (orig_num) {
    case SYS_clone_x86:
    case SYS_clone3_x86:
        return CHROME_SYSCALL_TAIL_CLONE;
    case SYS_wait4_x86:
    case SYS_waitid_x86:
        return CHROME_SYSCALL_TAIL_WAIT;
    case SYS_futex_x86:
    case SYS_futex_wait_x86:
    case SYS_futex_wake_x86:
    case SYS_futex_waitv_x86:
        return CHROME_SYSCALL_TAIL_FUTEX;
    case SYS_epoll_create:
    case SYS_epoll_create1:
    case SYS_epoll_create1_legacy:
    case SYS_epoll_ctl:
    case SYS_epoll_ctl_legacy:
    case SYS_epoll_ctl_old_x86:
    case SYS_epoll_wait:
    case SYS_epoll_wait_old_x86:
    case SYS_epoll_pwait:
    case SYS_epoll_pwait_legacy:
    case SYS_epoll_pwait2:
        return CHROME_SYSCALL_TAIL_EPOLL;
    case SYS_poll_x86:
    case SYS_ppoll_x86:
    case SYS_pselect6_x86:
        return CHROME_SYSCALL_TAIL_POLL;
    case SYS_open_x86:
    case SYS_openat_x86:
    case SYS_openat2_x86:
    case SYS_close_x86:
    case SYS_close_range_x86:
        return CHROME_SYSCALL_TAIL_OPEN;
    case SYS_fcntl_x86:
        return CHROME_SYSCALL_TAIL_FCNTL;
    case SYS_ioctl_x86:
        return CHROME_SYSCALL_TAIL_IOCTL;
    case SYS_mmap_x86:
    case SYS_mprotect_x86:
    case SYS_munmap_x86:
    case SYS_mremap_x86:
    case SYS_madvise_x86:
    case SYS_pkey_mprotect_x86:
        return CHROME_SYSCALL_TAIL_MMAP;
    case SYS_getpriority_x86:
    case SYS_setpriority_x86:
    case SYS_sched_yield_x86:
    case SYS_sched_setparam_x86:
    case SYS_sched_getparam_x86:
    case SYS_sched_setscheduler_x86:
    case SYS_sched_getscheduler_x86:
    case SYS_sched_get_priority_max_x86:
    case SYS_sched_get_priority_min_x86:
    case SYS_sched_rr_get_interval_x86:
    case SYS_sched_setaffinity_x86:
    case SYS_sched_getaffinity_x86:
    case SYS_sched_setattr_x86:
    case SYS_sched_getattr_x86:
    case SYS_prctl_x86:
        return CHROME_SYSCALL_TAIL_SCHED;
    case SYS_rt_sigaction_x86:
    case SYS_rt_sigprocmask_x86:
    case SYS_sigaltstack_x86:
    case SYS_tgkill_x86:
        return CHROME_SYSCALL_TAIL_SIGNAL;
    case SYS_socket_x86:
    case SYS_socketpair_x86:
    case SYS_connect_x86:
    case SYS_sendto_x86:
    case SYS_recvfrom_x86:
    case SYS_sendmsg_x86:
    case SYS_recvmsg_x86:
    case SYS_accept4_x86:
    case SYS_read_x86:
    case SYS_write_x86:
    case SYS_readv_x86:
    case SYS_writev_x86:
        return CHROME_SYSCALL_TAIL_IPC;
    default:
        return CHROME_SYSCALL_TAIL_OTHER;
    }
}

static struct chrome_syscall_tail_slot *
chrome_syscall_tail_slot(struct thread *p)
{
    int empty = -1;

    if (p == NULL)
        return NULL;
    for (int i = 0; i < CHROME_SYSCALL_TAIL_SLOTS; i++) {
        int pid = __atomic_load_n(&chrome_tail_slots[i].pid,
                                  __ATOMIC_RELAXED);
        if (pid == p->pid)
            return &chrome_tail_slots[i];
        if (pid == 0 && empty < 0)
            empty = i;
    }
    if (empty >= 0) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&chrome_tail_slots[empty].pid,
                                        &expected, p->pid, 0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return &chrome_tail_slots[empty];
    }
    return NULL;
}

static void chrome_syscall_tail_record(struct thread *p, int orig_num, int num,
                                       uint64 a0, uint64 a1, uint64 a2,
                                       uint64 a3, uint64 a4, uint64 a5,
                                       uint64 ret, uint64 start_ticks)
{
    struct chrome_syscall_tail_slot *slot = chrome_syscall_tail_slot(p);
    struct chrome_syscall_tail_entry *entry;
    uint64 seq;

    if (slot == NULL)
        return;
    seq = __atomic_fetch_add(&slot->next_seq, 1, __ATOMIC_RELAXED);
    entry = &slot->entries[seq % CHROME_SYSCALL_TAIL_DEPTH];
    entry->seq = seq;
    entry->ticks = start_ticks;
    entry->elapsed_us = chrome_trace_ticks_to_us(r_time() - start_ticks);
    entry->a0 = a0;
    entry->a1 = a1;
    entry->a2 = a2;
    entry->a3 = a3;
    entry->a4 = a4;
    entry->a5 = a5;
    entry->ret = ret;
    entry->orig_num = orig_num;
    entry->num = num;
    entry->pid = p->pid;
    entry->tgid = p->tgid;
    entry->cpu = p->sched_entity ? p->sched_entity->cpu_id : -1;
    entry->family = chrome_syscall_tail_family_for(orig_num);
}

static void chrome_syscall_tail_dump_fd(const char *prefix,
                                        const struct chrome_syscall_tail_entry *e)
{
    int fd = (int)e->a0;

    if (current == NULL || current->fdtable == NULL)
        return;
    if (e->family != CHROME_SYSCALL_TAIL_IOCTL &&
        e->family != CHROME_SYSCALL_TAIL_FCNTL)
        return;
    if (fd < 0 || fd >= NOFILE)
        return;

    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL) {
        printf("%s fd=%d fdpath=<closed>\n", prefix, fd);
        return;
    }

    char anon_path[96];
    const char *path = f->opened_path != NULL ? f->opened_path : NULL;
    if (path == NULL && f->ops != NULL && f->ops->readlink != NULL) {
        ssize_t n = f->ops->readlink(f, anon_path, sizeof(anon_path) - 1);
        if (n > 0) {
            if ((size_t)n >= sizeof(anon_path))
                n = sizeof(anon_path) - 1;
            anon_path[n] = '\0';
            path = anon_path;
        }
    }
    if (path == NULL)
        path = chrome_trace_file_kind_name(f);
    printf("%s fd=%d fdpath=%s fkind=%s flags=0x%x file=%p ref=%d\n",
           prefix, fd, path, chrome_trace_file_kind_name(f), f->f_flags,
           f, f->ref_count);
    vfs_fput(f);
}

static int chrome_syscall_tail_is_errno(uint64 ret)
{
    return ret >= (uint64)-4095 && ret != 0;
}

static void chrome_syscall_tail_dump_vma(const char *slot, vma_t *vma,
                                         uint64 addr)
{
    if (vma == NULL) {
        printf("chrome-syscall-tail-vma: slot=%s none addr=0x%lx\n",
               slot, addr);
        return;
    }

    struct vfs_inode *inode =
        vma->file != NULL ? vma->file->inode.inode : NULL;
    printf("chrome-syscall-tail-vma: slot=%s addr=0x%lx "
           "map=[0x%lx-0x%lx) %c%c%c %s flags=0x%lx pgoff=0x%lx "
           "file=%p ino=%lu size=%lld path=%s addr_in=%d\n",
           slot, addr, vma->start, vma->end,
           (vma->flags & PROT_READ) ? 'r' : '-',
           (vma->flags & PROT_WRITE) ? 'w' : '-',
           (vma->flags & PROT_EXEC) ? 'x' : '-',
           (vma->flags & VMA_FLAG_SHARED) ? "shared" : "private",
           vma->flags, vma->pgoff, (void *)vma->file,
           inode != NULL ? inode->ino : 0,
           inode != NULL ? inode->size : 0,
           chrome_thread_vma_path(vma),
           addr >= vma->start && addr < vma->end);
}

static void chrome_syscall_tail_dump_mprotect_vmas(
    const struct chrome_syscall_tail_entry *e)
{
    if (current == NULL || current->vm == NULL || e == NULL)
        return;
    if (e->orig_num != SYS_mprotect_x86 &&
        e->orig_num != SYS_pkey_mprotect_x86)
        return;
    if (!chrome_syscall_tail_is_errno(e->ret))
        return;

    uint64 addr = e->a0;
    vm_rlock(current->vm);
    vma_t *hit = vm_find_area(current->vm, addr);
    vma_t *left = hit != NULL ? (vma_t *)mt_prev(&current->vm->vm_mt,
                                                  hit->start, 0) :
        (vma_t *)mt_prev(&current->vm->vm_mt, addr, 0);
    uint64 next_idx = addr;
    vma_t *right = hit != NULL ? (vma_t *)mt_next(&current->vm->vm_mt,
                                                   hit->end - 1, MAPLE_MAX) :
        (vma_t *)mt_find(&current->vm->vm_mt, &next_idx, MAPLE_MAX);

    printf("chrome-syscall-tail-mprotect-fail: seq=%lu addr=0x%lx "
           "len=0x%lx prot=0x%lx ret=0x%lx errno=%ld\n",
           e->seq, e->a0, e->a1, e->a2, e->ret, -(long)(int64)e->ret);
    chrome_syscall_tail_dump_vma("left", left, addr);
    chrome_syscall_tail_dump_vma("hit", hit, addr);
    chrome_syscall_tail_dump_vma("right", right, addr);
    vm_runlock(current->vm);
}

void chrome_syscall_tail_dump_current(const char *reason)
{
    struct thread *p = current;
    struct chrome_syscall_tail_slot *slot;
    uint64 next_seq;
    uint64 first_seq;
    uint64 family_counts[CHROME_SYSCALL_TAIL_MAX] = {0};

    if (!chrome_syscall_tail_trace_enabled() ||
        !chrome_syscall_trace_process(p))
        return;

    slot = chrome_syscall_tail_slot(p);
    if (slot == NULL)
        return;

    next_seq = __atomic_load_n(&slot->next_seq, __ATOMIC_RELAXED);
    first_seq = next_seq > CHROME_SYSCALL_TAIL_DEPTH ?
        next_seq - CHROME_SYSCALL_TAIL_DEPTH : 0;

    printf("chrome-syscall-tail-summary: reason=%s pid=%d tgid=%d name=%s "
           "recorded=%lu first_seq=%lu next_seq=%lu depth=%d\n",
           reason ? reason : "-", p->pid, p->tgid, p->name,
           next_seq - first_seq, first_seq, next_seq,
           CHROME_SYSCALL_TAIL_DEPTH);

    for (uint64 seq = first_seq; seq < next_seq; seq++) {
        struct chrome_syscall_tail_entry e =
            slot->entries[seq % CHROME_SYSCALL_TAIL_DEPTH];

        if (e.seq != seq)
            continue;
        if (e.family >= 0 && e.family < CHROME_SYSCALL_TAIL_MAX)
            family_counts[e.family]++;
        printf("chrome-syscall-tail: reason=%s seq=%lu rel=%ld pid=%d "
               "tgid=%d cpu=%d family=%s orig=%d num=%d(%s) us=%lu "
               "a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx a4=0x%lx "
               "a5=0x%lx ret=0x%lx\n",
               reason ? reason : "-", e.seq, (long)e.seq - (long)next_seq,
               e.pid, e.tgid, e.cpu,
               chrome_syscall_tail_family_name(e.family), e.orig_num, e.num,
               x86_syscall_trace_name(e.orig_num), e.elapsed_us, e.a0, e.a1,
               e.a2, e.a3, e.a4, e.a5, e.ret);
        chrome_syscall_tail_dump_fd("chrome-syscall-tail-fd", &e);
        chrome_syscall_tail_dump_mprotect_vmas(&e);
    }

    printf("chrome-syscall-tail-families: reason=%s pid=%d tgid=%d "
           "clone=%lu wait=%lu futex=%lu epoll=%lu poll=%lu open=%lu "
           "fcntl=%lu ioctl=%lu mmap=%lu sched=%lu signal=%lu ipc=%lu "
           "other=%lu\n",
           reason ? reason : "-", p->pid, p->tgid,
           family_counts[CHROME_SYSCALL_TAIL_CLONE],
           family_counts[CHROME_SYSCALL_TAIL_WAIT],
           family_counts[CHROME_SYSCALL_TAIL_FUTEX],
           family_counts[CHROME_SYSCALL_TAIL_EPOLL],
           family_counts[CHROME_SYSCALL_TAIL_POLL],
           family_counts[CHROME_SYSCALL_TAIL_OPEN],
           family_counts[CHROME_SYSCALL_TAIL_FCNTL],
           family_counts[CHROME_SYSCALL_TAIL_IOCTL],
           family_counts[CHROME_SYSCALL_TAIL_MMAP],
           family_counts[CHROME_SYSCALL_TAIL_SCHED],
           family_counts[CHROME_SYSCALL_TAIL_SIGNAL],
           family_counts[CHROME_SYSCALL_TAIL_IPC],
           family_counts[CHROME_SYSCALL_TAIL_OTHER]);
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
    int orig_num = num;
    uint64 a0 = p->trapframe->trapframe.rdi;
    uint64 a1 = p->trapframe->trapframe.rsi;
    uint64 a2 = p->trapframe->trapframe.rdx;
    uint64 a3 = p->trapframe->trapframe.r10;
    uint64 a4 = p->trapframe->trapframe.r8;
    uint64 a5 = p->trapframe->trapframe.r9;
    static int chrome_syscall_trace_count;
    static int chrome_syscall_enter_trace_count;
    static int chrome_syscall_slow_trace_count;
    static int chrome_syscall_progress_trace_count;
    static int chrome_ppoll_trace_count;
    int chrome_process = chrome_syscall_trace_process(p);
    int trace = chrome_syscall_trace_enabled() &&
                chrome_process &&
                chrome_syscall_trace_interesting(orig_num, a1) &&
                chrome_syscall_trace_count < chrome_syscall_trace_limit();
    int enter_trace = chrome_syscall_enter_trace_enabled() &&
                      chrome_process &&
                      chrome_syscall_enter_trace_interesting(orig_num, a1) &&
                      chrome_syscall_enter_trace_count <
                          chrome_syscall_enter_trace_limit();
    int slow_trace = chrome_syscall_slow_trace_enabled() &&
                     chrome_process &&
                     chrome_syscall_slow_trace_count <
                         chrome_syscall_slow_trace_limit();
    uint64 slow_start = slow_trace ? r_time() : 0;
    int progress_trace = chrome_syscall_progress_trace_enabled() &&
                         chrome_process &&
                         chrome_syscall_progress_trace_count <
                             chrome_syscall_progress_limit();
    int tail_trace = chrome_syscall_tail_trace_enabled() && chrome_process;
    uint64 tail_start = tail_trace ? r_time() : 0;
    int ppoll_trace = chrome_ppoll_trace_enabled() &&
                      orig_num == SYS_ppoll_x86 &&
                      chrome_lifecycle_kernel_trace_process_match(p, 0, 1) &&
                      chrome_ppoll_trace_count < chrome_ppoll_trace_limit();
    int ppoll_trace_seq = 0;
    maybe_start_chrome_thread_dump(p);
    /*
     * Some musl x86_64 assembly helpers use native Linux syscall numbers
     * directly.  Most libc entry points go through xv6's generated syscall
     * numbers, but pthread self-teardown reaches __unmapself, which hardcodes
     * Linux munmap(11) and exit(60).  Route those specific shapes before the
     * compact xv6 table interprets them as sleep(11) or ftruncate(60).
     */
    if (num == 11 && looks_like_linux_munmap(a0, a1)) {
        num = SYS_munmap;
    } else if (num == 60 && (p->clone_flags & CLONE_THREAD)) {
        num = SYS_exit;
    }
    if (enter_trace) {
        chrome_syscall_enter_trace_count++;
        chrome_syscall_trace_line("enter", p, orig_num, num, a0, a1, a2, a3,
                                  a4, a5, 0, 0);
        chrome_syscall_trace_details(orig_num, a0, a1, a2);
        if (orig_num == SYS_poll_x86 || orig_num == SYS_ppoll_x86)
            chrome_syscall_trace_pollfds(a0, a1);
    }
    if (ppoll_trace) {
        uint32 roles = p->thread_group ?
            __atomic_load_n(&p->thread_group->chrome_trace_roles,
                            __ATOMIC_RELAXED) : 0;
        ppoll_trace_seq =
            __atomic_add_fetch(&chrome_ppoll_trace_count, 1,
                               __ATOMIC_RELAXED);
        printf("chrome-ppoll-trace: phase=enter seq=%d pid=%d tgid=%d "
               "roles=0x%x name=%s fds=0x%lx nfds=%lu tmo=0x%lx "
               "sigmask=0x%lx sigsetsize=%lu\n",
               ppoll_trace_seq, p->pid, p->tgid, roles, p->name,
               a0, a1, a2, a3, a4);
        chrome_syscall_trace_pollfds(a0, a1);
    }
    if (num >= 0 && num < (int)NELEM(syscalls) && syscalls[num]) {
        p->trapframe->trapframe.rax = syscalls[num]();
    } else {
#ifdef ENABLE_LEGACY_XV6_SYSCALL_ALIAS
        int legacy = legacy_xv6_syscall_alias(num);
        if (legacy > 0 && legacy < (int)NELEM(syscalls) && syscalls[legacy]) {
            p->trapframe->trapframe.rax = syscalls[legacy]();
        } else {
            p->trapframe->trapframe.rax = (uint64)-ENOSYS;
        }
#else
        p->trapframe->trapframe.rax = (uint64)-ENOSYS;
#endif
    }
    if (ppoll_trace) {
        uint32 roles = p->thread_group ?
            __atomic_load_n(&p->thread_group->chrome_trace_roles,
                            __ATOMIC_RELAXED) : 0;
        printf("chrome-ppoll-trace: phase=return seq=%d pid=%d tgid=%d "
               "roles=0x%x name=%s ret=0x%lx\n",
               ppoll_trace_seq, p->pid, p->tgid, roles, p->name,
               p->trapframe->trapframe.rax);
        chrome_syscall_trace_pollfds(a0, a1);
    }
    if (tail_trace) {
        chrome_syscall_tail_record(p, orig_num, num, a0, a1, a2, a3, a4, a5,
                                   p->trapframe->trapframe.rax, tail_start);
    }
    if (trace) {
        uint64 ret = p->trapframe->trapframe.rax;
        chrome_syscall_trace_count++;
        chrome_syscall_trace_line("return", p, orig_num, num, a0, a1, a2, a3,
                                  a4, a5, ret, 1);
        chrome_syscall_trace_details(orig_num, a0, a1, a2);
    }
    if (slow_trace) {
        uint64 elapsed = r_time() - slow_start;
        uint64 elapsed_us = chrome_trace_ticks_to_us(elapsed);

        if (elapsed_us >= chrome_syscall_slow_trace_threshold_us()) {
            chrome_syscall_slow_trace_count++;
            chrome_syscall_slow_trace_line(p, orig_num, num, a0, a1, a2, a3,
                                           a4, a5,
                                           p->trapframe->trapframe.rax,
                                           elapsed);
            chrome_syscall_trace_details(orig_num, a0, a1, a2);
            if (orig_num == SYS_read_x86)
                chrome_syscall_trace_fd_arg("fd", a0);
            if (orig_num == SYS_poll_x86 || orig_num == SYS_ppoll_x86)
                chrome_syscall_trace_pollfds(a0, a1);
        }
    }
    if (progress_trace) {
        struct chrome_syscall_progress_slot *slot =
            chrome_syscall_progress_slot(p);
        int interval = chrome_syscall_progress_interval();

        if (slot != NULL && interval > 0) {
            uint64 count = __atomic_add_fetch(&slot->count, 1,
                                              __ATOMIC_RELAXED);
            if ((count % (uint64)interval) == 0) {
                chrome_syscall_progress_trace_count++;
                chrome_syscall_progress_trace_line(
                    p, orig_num, num, a0, a1, a2,
                    p->trapframe->trapframe.rax, count);
                chrome_syscall_trace_details(orig_num, a0, a1, a2);
            }
        }
    }
}
