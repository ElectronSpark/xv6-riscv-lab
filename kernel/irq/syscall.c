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
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_uptime(void);
extern uint64 sys_sigaction(void);
extern uint64 sys_sigpending(void);
extern uint64 sys_sigprocmask(void);
extern uint64 sys_sigreturn(void);
// extern uint64 sys_sigalarm(void);
extern uint64 sys_pause(void);
extern uint64 sys_gettid(void);
extern uint64 sys_exit_group(void);
extern uint64 sys_tgkill(void);
extern uint64 sys_vfork(void);

extern uint64 sys_memstat(void);
extern uint64 sys_dumpproc(void);
extern uint64 sys_dumpchan(void);
extern uint64 sys_dumppcache(void);
extern uint64 sys_dumprq(void);
extern uint64 sys_kernbase(void);
extern uint64 sys_dumpinode(void);

// 900
extern uint64 sys_sync(void);

// VFS syscalls
extern uint64 sys_vfs_dup(void);
extern uint64 sys_vfs_read(void);
extern uint64 sys_vfs_write(void);
extern uint64 sys_vfs_close(void);
extern uint64 sys_vfs_fstat(void);
extern uint64 sys_vfs_open(void);
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
    [SYS_gettid] sys_gettid,
    [SYS_exit_group] sys_exit_group,
    [SYS_tgkill] sys_tgkill,
    [SYS_vfork] sys_vfork,
    [SYS_memstat] sys_memstat,
    [SYS_dumpproc] sys_dumpproc,
    [SYS_dumpchan] sys_dumpchan,
    [SYS_dumppcache] sys_dumppcache,
    [SYS_dumprq] sys_dumprq,
    [SYS_kernbase] sys_kernbase,
    [SYS_dumpinode] sys_dumpinode,
    [SYS_sync] sys_sync,
    // VFS extended syscalls (1000+)
    [SYS_vfs_dup] sys_vfs_dup,
    [SYS_vfs_read] sys_vfs_read,
    [SYS_vfs_write] sys_vfs_write,
    [SYS_vfs_close] sys_vfs_close,
    [SYS_vfs_fstat] sys_vfs_fstat,
    [SYS_vfs_open] sys_vfs_open,
    [SYS_vfs_mkdir] sys_vfs_mkdir,
    [SYS_vfs_mknod] sys_vfs_mknod,
    [SYS_vfs_unlink] sys_vfs_unlink,
    [SYS_vfs_link] sys_vfs_link,
    [SYS_vfs_symlink] sys_vfs_symlink,
    [SYS_vfs_chdir] sys_vfs_chdir,
    [SYS_vfs_pipe] sys_vfs_pipe,
    [SYS_vfs_connect] sys_vfs_connect,
    [SYS_getdents] sys_getdents,
    [SYS_chroot] sys_chroot,
    [SYS_mount] sys_mount,
    [SYS_umount] sys_umount,
    [SYS_getcwd] sys_getcwd,
};

void syscall(void) {
    int num;
    struct thread *p = current;

    num = p->trapframe->trapframe.a7;

    if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
        p->trapframe->trapframe.a0 = syscalls[num]();
    } else {
        printf("%d %s: unknown sys call %d\n", p->pid, p->name, num);
        p->trapframe->trapframe.a0 = -1;
    }
}
