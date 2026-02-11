#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/sched.h"
#include "timer/timer.h"
#include <mm/vm.h>
#include "clone_flags.h"
#include "signal.h"
#include "errno.h"

uint64 sys_exit(void) {
    int n;
    argint(0, &n);
    exit(n);
    return 0; // not reached
}

uint64 sys_getpid(void) { return thread_tgid(current); }

// gettid() returns the caller's thread ID (TID), which is the kernel-level
// unique identifier. In a single-threaded process, TID == TGID == PID.
// In a multi-threaded process (CLONE_THREAD), TID != TGID.
uint64 sys_gettid(void) { return current->pid; }

// exit_group() terminates all threads in the calling thread's thread group.
// This is what C library exit() and _exit() should call.
uint64 sys_exit_group(void) {
    int n;
    argint(0, &n);
    thread_group_exit(current, n);
    return 0; // not reached
}

// tgkill() sends a signal to a specific thread within a thread group.
// This provides race-free signal delivery by verifying the thread
// still belongs to the specified thread group.
uint64 sys_tgkill(void) {
    int tgid, tid, sig;
    argint(0, &tgid);
    argint(1, &tid);
    argint(2, &sig);
    return tgkill(tgid, tid, sig);
}

// vfork() — dedicated syscall so the userspace wrapper is pure assembly
// (ecall + ret, no stack usage). This avoids corrupting the parent's
// stack frame, which is shared with the child via CLONE_VM.
uint64 sys_vfork(void) {
    struct clone_args args = {
        .flags = CLONE_VM | CLONE_VFORK,
        .stack = 0,
        .stack_size = 0,
        .entry = 0,
        .esignal = SIGCHLD,
        .tls = 0,
        .ctid = 0,
        .ptid = 0,
    };
    return thread_clone(&args);
}

uint64 sys_clone(void) {
    uint64 uargs;
    argaddr(0, &uargs);

    struct clone_args args = {0};
    if (uargs == 0) {
        // No args provided - default to fork behavior
        args.flags = SIGCHLD;
        args.esignal = SIGCHLD;
    } else {
        if (vm_copyin(current->vm, &args, uargs, sizeof(args)) < 0) {
            return -EFAULT;
        }
        // If esignal not explicitly set, extract from low bits of flags (Linux
        // convention)
        if (args.esignal == 0) {
            args.esignal = args.flags & 0xFF;
        }
    }
    return thread_clone(&args);
}

uint64 sys_wait(void) {
    uint64 p;
    argaddr(0, &p);
    return wait(p);
}

uint64 sys_sbrk(void) {
    uint64 addr;
    int64 n;

    argint64(0, &n);
    vma_t *vma = current->vm->heap;
    if (vma == NULL) {
        return -1; // No heap VMA found
    }
    addr = current->vm->heap->start + current->vm->heap_size;
    if (vm_growheap(current->vm, n) < 0) {
        return -1;
    }
    return addr;
}

uint64 sys_sleep(void) {
    int n;
    argint(0, &n);
    if (n < 0)
        n = 0;
    sleep_ms(n);
    return 0;
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) { return get_jiffs(); }

// return the physical memory start address (KERNBASE)
// for user-space tests that need to verify they can't access kernel memory
uint64 sys_kernbase(void) { return __physical_memory_start; }
