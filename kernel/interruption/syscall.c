#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"
#include "printf.h"
#include "vm.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  // if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
  //   return -1;
  if(vm_copyin(p->vm, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if(vm_copyinstr(p->vm, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

STATIC uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_fork(void);
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

extern uint64 sys_memstat(void);
extern uint64 sys_dumpproc(void);
extern uint64 sys_dumpchan(void);
extern uint64 sys_dumppcache(void);

//900
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
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_vfs_pipe,     // VFS
[SYS_read]    sys_vfs_read,     // VFS
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_vfs_fstat,    // VFS
[SYS_chdir]   sys_vfs_chdir,    // VFS
[SYS_dup]     sys_vfs_dup,      // VFS
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_vfs_open,     // VFS
[SYS_write]   sys_vfs_write,    // VFS
[SYS_mknod]   sys_vfs_mknod,    // VFS
[SYS_unlink]  sys_vfs_unlink,   // VFS
[SYS_link]    sys_vfs_link,     // VFS
[SYS_mkdir]   sys_vfs_mkdir,    // VFS
[SYS_close]   sys_vfs_close,    // VFS
[SYS_connect] sys_vfs_connect,  // VFS
[SYS_symlink] sys_vfs_symlink,  // VFS
// [SYS_sigalarm] sys_sigalarm,
[SYS_sigaction] sys_sigaction,
[SYS_sigreturn] sys_sigreturn,
[SYS_sigpending] sys_sigpending,
[SYS_sigprocmask] sys_sigprocmask,
[SYS_pause] sys_pause,
[SYS_memstat] sys_memstat,
[SYS_dumpproc] sys_dumpproc,
[SYS_dumpchan] sys_dumpchan,
[SYS_dumppcache] sys_dumppcache,
[SYS_sync]    sys_sync,
};

// Handle VFS syscalls (1000+)
static uint64 handle_vfs_syscall(int num) {
  switch (num) {
    case SYS_vfs_dup:     return sys_vfs_dup();
    case SYS_vfs_read:    return sys_vfs_read();
    case SYS_vfs_write:   return sys_vfs_write();
    case SYS_vfs_close:   return sys_vfs_close();
    case SYS_vfs_fstat:   return sys_vfs_fstat();
    case SYS_vfs_open:    return sys_vfs_open();
    case SYS_vfs_mkdir:   return sys_vfs_mkdir();
    case SYS_vfs_mknod:   return sys_vfs_mknod();
    case SYS_vfs_unlink:  return sys_vfs_unlink();
    case SYS_vfs_link:    return sys_vfs_link();
    case SYS_vfs_symlink: return sys_vfs_symlink();
    case SYS_vfs_chdir:   return sys_vfs_chdir();
    case SYS_vfs_pipe:    return sys_vfs_pipe();
    case SYS_vfs_connect: return sys_vfs_connect();
    case SYS_getdents:    return sys_getdents();
    case SYS_chroot:      return sys_chroot();
    case SYS_mount:       return sys_mount();
    case SYS_umount:      return sys_umount();
    default:              return (uint64)-1;
  }
}

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  
  // Handle VFS syscalls (1000+)
  if (num >= 1000) {
    p->trapframe->a0 = handle_vfs_syscall(num);
    if (p->trapframe->a0 == (uint64)-1) {
      printf("%d %s: unknown vfs sys call %d\n", p->pid, p->name, num);
    }
    return;
  }
  
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // Use num to lookup the system call function for num, call it,
    // and store its return value in p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
