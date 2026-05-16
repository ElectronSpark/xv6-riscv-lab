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

extern uint64 sys_memstat(void);
extern uint64 sys_dumpproc(void);
extern uint64 sys_dumpchan(void);
extern uint64 sys_dumppcache(void);
extern uint64 sys_dumprq(void);
extern uint64 sys_kernbase(void);
extern uint64 sys_dumpinode(void);

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
extern uint64 sys_statfs(void);
extern uint64 sys_chroot(void);
extern uint64 sys_mount(void);
extern uint64 sys_umount(void);
extern uint64 sys_getcwd(void);
extern uint64 sys_vfs_ioctl(void);
extern uint64 sys_tcgetattr(void);
extern uint64 sys_tcsetattr(void);
extern uint64 sys_vfs_poll(void);
extern uint64 sys_fallocate(void);

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
    [SYS_connect] sys_vfs_connect, // VFS
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
    [SYS_memstat] sys_memstat,
    [SYS_dumpproc] sys_dumpproc,
    [SYS_dumpchan] sys_dumpchan,
    [SYS_dumppcache] sys_dumppcache,
    [SYS_dumprq] sys_dumprq,
    [SYS_kernbase] sys_kernbase,
    [SYS_dumpinode] sys_dumpinode,
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
    [SYS_ftruncate] sys_vfs_ftruncate,
    [SYS_fallocate] sys_fallocate,
    [SYS_gettimeofday] sys_gettimeofday,
    [SYS_waitpid] sys_waitpid,
    [SYS_nanosleep] sys_nanosleep,
    [SYS_uname] sys_uname,
    [SYS_getdents] sys_getdents,
    [SYS_statfs] sys_statfs,
    [SYS_chroot] sys_chroot,
    [SYS_mount] sys_mount,
    [SYS_umount] sys_umount,
    [SYS_getcwd] sys_getcwd,
};

static int gpu_syscall_trace_process(struct thread *p)
{
    return p != NULL;
}

static const char *gpu_syscall_name(int num)
{
    switch (num) {
    case SYS_open: return "open";
    case SYS_close: return "close";
    case SYS_fstat: return "fstat";
    case SYS_ioctl: return "ioctl";
    case SYS_fcntl: return "fcntl";
    case SYS_stat: return "stat";
    case SYS_mmap: return "mmap";
    case SYS_munmap: return "munmap";
    case SYS_mprotect: return "mprotect";
    case SYS_mremap: return "mremap";
    case SYS_msync: return "msync";
    case SYS_madvise: return "madvise";
    case SYS_ftruncate: return "ftruncate";
    case SYS_lstat: return "lstat";
    case SYS_openat: return "openat";
    case SYS_fstatat: return "fstatat";
    case SYS_memfd_create: return "memfd_create";
    case SYS_statx: return "statx";
    case SYS_fstatfs: return "fstatfs";
    case SYS_open_x86: return "open_x86";
    case SYS_close_x86: return "close_x86";
    case SYS_fstat_x86: return "fstat_x86";
    case SYS_ioctl_x86: return "ioctl_x86";
    case SYS_fcntl_x86: return "fcntl_x86";
    case SYS_stat_x86: return "stat_x86";
    case SYS_mmap_x86: return "mmap_x86";
    case SYS_munmap_x86: return "munmap_x86";
    case SYS_mprotect_x86: return "mprotect_x86";
    case SYS_mremap_x86: return "mremap_x86";
    case SYS_msync_x86: return "msync_x86";
    case SYS_madvise_x86: return "madvise_x86";
    case SYS_ftruncate_x86: return "ftruncate_x86";
    case SYS_openat_x86: return "openat_x86";
    case SYS_newfstatat_x86: return "newfstatat_x86";
    case SYS_memfd_create_x86: return "memfd_create_x86";
    case SYS_statx_x86: return "statx_x86";
    case SYS_fstatfs_x86: return "fstatfs_x86";
    default: return "?";
    }
}

static int gpu_syscall_trace_interesting(int num)
{
    switch (num) {
    case SYS_open:
    case SYS_close:
    case SYS_fstat:
    case SYS_ioctl:
    case SYS_fcntl:
    case SYS_stat:
    case SYS_mmap:
    case SYS_munmap:
    case SYS_mprotect:
    case SYS_mremap:
    case SYS_msync:
    case SYS_madvise:
    case SYS_ftruncate:
    case SYS_lstat:
    case SYS_openat:
    case SYS_fstatat:
    case SYS_memfd_create:
    case SYS_statx:
    case SYS_fstatfs:
    case SYS_open_x86:
    case SYS_close_x86:
    case SYS_fstat_x86:
    case SYS_ioctl_x86:
    case SYS_fcntl_x86:
    case SYS_stat_x86:
    case SYS_mmap_x86:
    case SYS_munmap_x86:
    case SYS_mprotect_x86:
    case SYS_mremap_x86:
    case SYS_msync_x86:
    case SYS_madvise_x86:
    case SYS_ftruncate_x86:
    case SYS_openat_x86:
    case SYS_newfstatat_x86:
    case SYS_memfd_create_x86:
    case SYS_statx_x86:
    case SYS_fstatfs_x86:
        return 1;
    default:
        return 0;
    }
}

void syscall(void) {
    int num;
    struct thread *p = current;
    uint64 a0, a1, a2, a3, a4, a5, ret;
    int trace;
    static int gpu_syscall_trace_count;

    num = p->trapframe->trapframe.a7;
    a0 = p->trapframe->trapframe.a0;
    a1 = p->trapframe->trapframe.a1;
    a2 = p->trapframe->trapframe.a2;
    a3 = p->trapframe->trapframe.a3;
    a4 = p->trapframe->trapframe.a4;
    a5 = p->trapframe->trapframe.a5;
    trace = gpu_syscall_trace_process(p) &&
            gpu_syscall_trace_interesting(num) &&
            gpu_syscall_trace_count < 512;

    if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
        ret = syscalls[num]();
        p->trapframe->trapframe.a0 = ret;
    } else {
        printf("%d %s: unknown sys call %d\n", p->pid, p->name, num);
        ret = (uint64)-ENOSYS;
        p->trapframe->trapframe.a0 = ret;
        trace = gpu_syscall_trace_process(p) &&
                gpu_syscall_trace_count < 512;
    }

    if (trace) {
        gpu_syscall_trace_count++;
        printf("gpu-syscall: pid=%d name=%s num=%d(%s) a0=0x%lx a1=0x%lx "
               "a2=0x%lx a3=0x%lx a4=0x%lx a5=0x%lx ret=0x%lx\n",
               p->pid, p->name, num, gpu_syscall_name(num),
               a0, a1, a2, a3, a4, a5, ret);
    }
}
