/**
 * @file syscall.c
 * @brief x86_64 system call argument fetching.
 *
 * On x86_64 (Linux convention), syscall arguments are passed in:
 *   arg0=RDI, arg1=RSI, arg2=RDX, arg3=R10, arg4=R8, arg5=R9
 * (R10 replaces RCX, which is clobbered by SYSCALL instruction.)
 */

#include "types.h"
#include "proc/thread.h"
#include "mm/vm.h"
#include "printf.h"

int fetchaddr(uint64 addr, uint64 *ip) {
    struct thread *p = current;
    if (!p || !p->vm)
        return -1;
    return vm_copyin(p->vm, (char *)ip, addr, sizeof(*ip));
}

int fetchstr(uint64 addr, char *buf, int max) {
    struct thread *p = current;
    if (!p || !p->vm)
        return -1;
    return vm_copyinstr(p->vm, buf, addr, max) < 0 ? -1 : 0;
}

static uint64 argraw(int n) {
    struct thread *p = current;
    switch (n) {
    case 0: return p->trapframe->trapframe.rdi;
    case 1: return p->trapframe->trapframe.rsi;
    case 2: return p->trapframe->trapframe.rdx;
    case 3: return p->trapframe->trapframe.r10;
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

