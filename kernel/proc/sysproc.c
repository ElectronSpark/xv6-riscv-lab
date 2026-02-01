#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "timer/timer.h"
#include <mm/vm.h>
#include "clone_flags.h"
#include "errno.h"

uint64 sys_exit(void) {
    int n;
    argint(0, &n);
    exit(n);
    return 0; // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_clone(void) {
    uint64 uargs;
    argaddr(0, &uargs);

    struct clone_args args = {0};
    if (uargs == 0) {
        // No args provided - default to fork behavior
        args.flags = SIGCHLD;
        args.esignal = SIGCHLD;
    } else {
        if (vm_copyin(myproc()->vm, &args, uargs, sizeof(args)) < 0) {
            return -EFAULT;
        }
        // If esignal not explicitly set, extract from low bits of flags (Linux convention)
        if (args.esignal == 0) {
            args.esignal = args.flags & 0xFF;
        }
    }
    return proc_clone(&args);
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
    vma_t *vma = myproc()->vm->heap;
    if (vma == NULL) {
        return -1; // No heap VMA found
    }
    addr = myproc()->vm->heap->start + myproc()->vm->heap_size;
    // printf("sbrk: addr %p n %d -> ", (void*)addr, n);
    if (vm_growheap(myproc()->vm, n) < 0) {
        // printf("growproc failed\n");
        return -1;
    }
    // printf("new addr %p\n", (void*)(myproc()->vm->heap->start +
    // myproc()->vm->heap_size));
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
