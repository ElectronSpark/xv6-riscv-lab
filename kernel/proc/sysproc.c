#include "types.h"
#include "string.h"
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
#include "proc/pgroup.h"
#include "tty/session.h"
#include "timer/goldfish_rtc.h"

struct __k_timespec {
    int64 tv_sec;
    int64 tv_nsec;
};

struct __k_timeval {
    int64 tv_sec;
    int64 tv_usec;
};

struct __k_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

uint64 sys_exit(void) {
    int n;
    argint(0, &n);
    exit(n);
    return 0; // not reached
}

uint64 sys_getpid(void) { return thread_tgid(current); }

uint64 sys_getuid(void)  { return 0; }  /* always root */
uint64 sys_geteuid(void) { return 0; }
uint64 sys_getgid(void)  { return 0; }
uint64 sys_getegid(void) { return 0; }
uint64 sys_setuid(void)  { return 0; }  /* silently succeed */
uint64 sys_setgid(void)  { return 0; }
uint64 sys_setreuid(void){ return 0; }
uint64 sys_setregid(void){ return 0; }

uint64 sys_getgroups(void) {
    /* a0 = size, a1 = list[]; if size==0 return count */
    int size;
    argint(0, &size);
    return 0;  /* root has 0 supplementary groups */
}

uint64 sys_setgroups(void) { return 0; }

uint64 sys_getppid(void) {
    if (current->parent == NULL) {
        return 1;
    }
    return thread_tgid(current->parent);
}

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

uint64 sys_waitpid(void) {
    int pid, options;
    uint64 status_addr;

    argint(0, &pid);
    argaddr(1, &status_addr);
    argint(2, &options);
    return waitpid(pid, status_addr, options);
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

uint64 sys_gettimeofday(void) {
    uint64 tv_addr;
    uint64 tz_addr;
    argaddr(0, &tv_addr);
    argaddr(1, &tz_addr);
    (void)tz_addr;

    if (tv_addr == 0) {
        return -EINVAL;
    }

    uint64 t = goldfish_rtc_read_ns();
    struct __k_timeval tv = {
        .tv_sec = t / NS_PER_SEC,
        .tv_usec = (int64)((t % NS_PER_SEC) / NS_PER_US),
    };

    if (either_copyout(1, tv_addr, &tv, sizeof(tv)) < 0) {
        return -EFAULT;
    }
    return 0;
}

uint64 sys_nanosleep(void) {
    uint64 req_addr, rem_addr;
    argaddr(0, &req_addr);
    argaddr(1, &rem_addr);

    if (req_addr == 0) {
        return -EINVAL;
    }

    struct __k_timespec req = {0};
    if (either_copyin(&req, 1, req_addr, sizeof(req)) < 0) {
        return -EFAULT;
    }

    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000LL) {
        return -EINVAL;
    }

    uint64 total_ns = (uint64)req.tv_sec * 1000000000ULL + (uint64)req.tv_nsec;
    uint64 ms = (total_ns + 999999ULL) / 1000000ULL;
    if (total_ns > 0 && ms == 0) {
        ms = 1;
    }
    sleep_ms(ms);

    if (rem_addr != 0) {
        struct __k_timespec rem = {0};
        if (either_copyout(1, rem_addr, &rem, sizeof(rem)) < 0) {
            return -EFAULT;
        }
    }
    return 0;
}

uint64 sys_uname(void) {
    uint64 addr;
    argaddr(0, &addr);
    if (addr == 0) {
        return -EINVAL;
    }

    struct __k_utsname u;
    memset(&u, 0, sizeof(u));
    safestrcpy(u.sysname, "xv6", sizeof(u.sysname));
    safestrcpy(u.nodename, "xv6", sizeof(u.nodename));
    safestrcpy(u.release, "0.1", sizeof(u.release));
    safestrcpy(u.version, "xv6-tmp", sizeof(u.version));
    safestrcpy(u.machine, "riscv64", sizeof(u.machine));

    if (either_copyout(1, addr, &u, sizeof(u)) < 0) {
        return -EFAULT;
    }
    return 0;
}

// return the physical memory start address (KERNBASE)
// for user-space tests that need to verify they can't access kernel memory
uint64 sys_kernbase(void) { return __physical_memory_start; }

/* ---- Process group / session syscalls ---- */

uint64 sys_setpgid(void) {
    int pid, pgid;
    argint(0, &pid);
    argint(1, &pgid);
    return pgroup_setpgid((pid_t)pid, (pid_t)pgid);
}

uint64 sys_getpgid(void) {
    int pid;
    argint(0, &pid);
    return pgroup_getpgid((pid_t)pid);
}

uint64 sys_setsid(void) { return session_setsid(); }

uint64 sys_getsid(void) {
    int pid;
    argint(0, &pid);
    return session_getsid((pid_t)pid);
}

uint64 sys_getrandom(void) {
    uint64 ubuf;
    int len;

    argaddr(0, &ubuf);
    argint(1, &len);

    if (len < 0) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }

    uint8 kbuf[64];
    int done = 0;
    while (done < len) {
        int chunk = len - done;
        if (chunk > (int)sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }

        random_fill_bytes(kbuf, chunk);
        if (either_copyout(1, ubuf + done, kbuf, chunk) < 0) {
            return done ? done : -EFAULT;
        }
        done += chunk;
    }

    return done;
}

// mmap/munmap/mprotect moved to kernel/mm/sysmm.c

/*
 * brk(addr) — set the program break (end of heap).
 *
 * If addr == 0, return the current break.
 * Otherwise, grow or shrink the heap to reach addr.
 * Returns the new break on success, or the old break on failure.
 */
uint64 sys_brk(void) {
    uint64 addr;
    argaddr(0, &addr);

    vm_t *vm = current->vm;
    vma_t *heap = vm->heap;
    if (heap == NULL)
        return (uint64)-ENOMEM;

    uint64 cur_brk = heap->start + vm->heap_size;

    if (addr == 0) {
        return cur_brk;
    }

    if (addr < heap->start)
        return cur_brk; // Cannot shrink below heap start

    int64 delta = (int64)(addr - cur_brk);
    if (delta == 0)
        return cur_brk;

    if (vm_growheap(vm, delta) < 0) {
        printf("sys_brk: FAIL pid=%d addr=0x%lx cur_brk=0x%lx delta=%ld\n",
               current->pid, addr, cur_brk, delta);
        return cur_brk; // Return old break on failure
    }

    return heap->start + vm->heap_size;
}

/*
 * set_tid_address(tidptr) — store the clear_child_tid pointer.
 *
 * When this thread exits, the kernel will:
 *   1. Write 0 to *tidptr
 *   2. futex_wake(tidptr, 1) to wake pthread_join waiters
 *
 * Returns the caller's TID.
 */
uint64 sys_set_tid_address(void) {
    uint64 tidptr;
    argaddr(0, &tidptr);
    current->clear_child_tid = tidptr;
    return current->pid;
}

/*
 * clock_gettime(clockid, tp) — get time from specified clock.
 */
uint64 sys_clock_gettime(void) {
    int clockid;
    uint64 tp_addr;
    argint(0, &clockid);
    argaddr(1, &tp_addr);

    if (tp_addr == 0)
        return -EINVAL;

    struct __k_timespec ts = {0};

    switch (clockid) {
    case 0: // CLOCK_REALTIME
    case 1: // CLOCK_MONOTONIC (treat same as realtime for now)
    {
        uint64 ns = goldfish_rtc_read_ns();
        ts.tv_sec = ns / 1000000000ULL;
        ts.tv_nsec = ns % 1000000000ULL;
        break;
    }
    default:
        return -EINVAL;
    }

    if (either_copyout(1, tp_addr, &ts, sizeof(ts)) < 0)
        return -EFAULT;

    return 0;
}

/*
 * clock_getres(clockid, res) — get resolution of specified clock.
 */
uint64 sys_clock_getres(void) {
    int clockid;
    uint64 res_addr;
    argint(0, &clockid);
    argaddr(1, &res_addr);

    if (clockid != 0 && clockid != 1)
        return -EINVAL;

    if (res_addr != 0) {
        struct __k_timespec res = {.tv_sec = 0, .tv_nsec = 1000}; // 1µs
        if (either_copyout(1, res_addr, &res, sizeof(res)) < 0)
            return -EFAULT;
    }
    return 0;
}
