#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/cred.h"
#include "proc/thread_group.h"
#include "proc/sched.h"
#include "timer/timer.h"
#include <mm/vm.h>
#include "clone_flags.h"
#include "signal.h"
#include "errno.h"
#include "accounting.h"
#include "proc/pgroup.h"
#include "tty/session.h"
#include "diag.h"
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

/* ── Credential syscalls ─────────────────────────────────────────────────── */

uint64 sys_getuid(void)  { return current->thread_group->uid;  }
uint64 sys_geteuid(void) { return current->thread_group->euid; }
uint64 sys_getgid(void)  { return current->thread_group->gid;  }
uint64 sys_getegid(void) { return current->thread_group->egid; }

uint64 sys_setuid(void) {
    int id;
    argint(0, &id);
    if (id < 0) return (uint64)-EINVAL;
    struct thread_group *tg = current->thread_group;
    if (tg->euid == 0) {
        /* Privileged: set all three */
        tg->uid = tg->euid = tg->suid = (uint32)id;
    } else if ((uint32)id == tg->uid || (uint32)id == tg->suid) {
        tg->euid = (uint32)id;
    } else {
        return (uint64)-EPERM;
    }
    return 0;
}

uint64 sys_setgid(void) {
    int id;
    argint(0, &id);
    if (id < 0) return (uint64)-EINVAL;
    struct thread_group *tg = current->thread_group;
    if (tg->euid == 0) {
        tg->gid = tg->egid = tg->sgid = (uint32)id;
    } else if ((uint32)id == tg->gid || (uint32)id == tg->sgid) {
        tg->egid = (uint32)id;
    } else {
        return (uint64)-EPERM;
    }
    return 0;
}

uint64 sys_setreuid(void) {
    int ruid, euid;
    argint(0, &ruid);
    argint(1, &euid);
    struct thread_group *tg = current->thread_group;
    uint32 old_ruid = tg->uid;

    if (ruid != -1) {
        if (tg->euid == 0 || (uint32)ruid == tg->uid ||
            (uint32)ruid == tg->euid) {
            tg->uid = (uint32)ruid;
        } else {
            return (uint64)-EPERM;
        }
    }
    if (euid != -1) {
        if (tg->euid == 0 || (uint32)euid == old_ruid ||
            (uint32)euid == tg->euid || (uint32)euid == tg->suid) {
            tg->euid = (uint32)euid;
        } else {
            /* Restore ruid on failure */
            tg->uid = old_ruid;
            return (uint64)-EPERM;
        }
    }
    /* If real UID was set, or effective UID was set to a value not equal
     * to the previous real UID, the saved UID is set to the new euid. */
    if (ruid != -1 || (euid != -1 && (uint32)euid != old_ruid))
        tg->suid = tg->euid;
    return 0;
}

uint64 sys_setregid(void) {
    int rgid, egid;
    argint(0, &rgid);
    argint(1, &egid);
    struct thread_group *tg = current->thread_group;
    uint32 old_rgid = tg->gid;

    if (rgid != -1) {
        if (tg->euid == 0 || (uint32)rgid == tg->gid ||
            (uint32)rgid == tg->egid) {
            tg->gid = (uint32)rgid;
        } else {
            return (uint64)-EPERM;
        }
    }
    if (egid != -1) {
        if (tg->euid == 0 || (uint32)egid == old_rgid ||
            (uint32)egid == tg->egid || (uint32)egid == tg->sgid) {
            tg->egid = (uint32)egid;
        } else {
            tg->gid = old_rgid;
            return (uint64)-EPERM;
        }
    }
    if (rgid != -1 || (egid != -1 && (uint32)egid != old_rgid))
        tg->sgid = tg->egid;
    return 0;
}

uint64 sys_setresuid(void) {
    int ruid, euid, suid;
    argint(0, &ruid);
    argint(1, &euid);
    argint(2, &suid);
    struct thread_group *tg = current->thread_group;

    /* Unprivileged callers may only set each id to one of the current
     * real, effective, or saved values.  Privileged (euid==0) may set
     * any value.  -1 means "leave unchanged". */
    if (ruid != -1) {
        if (tg->euid != 0 && (uint32)ruid != tg->uid &&
            (uint32)ruid != tg->euid && (uint32)ruid != tg->suid)
            return (uint64)-EPERM;
    }
    if (euid != -1) {
        if (tg->euid != 0 && (uint32)euid != tg->uid &&
            (uint32)euid != tg->euid && (uint32)euid != tg->suid)
            return (uint64)-EPERM;
    }
    if (suid != -1) {
        if (tg->euid != 0 && (uint32)suid != tg->uid &&
            (uint32)suid != tg->euid && (uint32)suid != tg->suid)
            return (uint64)-EPERM;
    }

    /* All permission checks passed — apply atomically */
    if (ruid != -1) tg->uid  = (uint32)ruid;
    if (euid != -1) tg->euid = (uint32)euid;
    if (suid != -1) tg->suid = (uint32)suid;
    return 0;
}

uint64 sys_setresgid(void) {
    int rgid, egid, sgid;
    argint(0, &rgid);
    argint(1, &egid);
    argint(2, &sgid);
    struct thread_group *tg = current->thread_group;

    if (rgid != -1) {
        if (tg->euid != 0 && (uint32)rgid != tg->gid &&
            (uint32)rgid != tg->egid && (uint32)rgid != tg->sgid)
            return (uint64)-EPERM;
    }
    if (egid != -1) {
        if (tg->euid != 0 && (uint32)egid != tg->gid &&
            (uint32)egid != tg->egid && (uint32)egid != tg->sgid)
            return (uint64)-EPERM;
    }
    if (sgid != -1) {
        if (tg->euid != 0 && (uint32)sgid != tg->gid &&
            (uint32)sgid != tg->egid && (uint32)sgid != tg->sgid)
            return (uint64)-EPERM;
    }

    if (rgid != -1) tg->gid  = (uint32)rgid;
    if (egid != -1) tg->egid = (uint32)egid;
    if (sgid != -1) tg->sgid = (uint32)sgid;
    return 0;
}

uint64 sys_getresuid(void) {
    uint64 ruid_addr, euid_addr, suid_addr;
    argaddr(0, &ruid_addr);
    argaddr(1, &euid_addr);
    argaddr(2, &suid_addr);
    struct thread_group *tg = current->thread_group;

    if (either_copyout(1, ruid_addr, &tg->uid, sizeof(uint32)) < 0)
        return (uint64)-EFAULT;
    if (either_copyout(1, euid_addr, &tg->euid, sizeof(uint32)) < 0)
        return (uint64)-EFAULT;
    if (either_copyout(1, suid_addr, &tg->suid, sizeof(uint32)) < 0)
        return (uint64)-EFAULT;
    return 0;
}

uint64 sys_getresgid(void) {
    uint64 rgid_addr, egid_addr, sgid_addr;
    argaddr(0, &rgid_addr);
    argaddr(1, &egid_addr);
    argaddr(2, &sgid_addr);
    struct thread_group *tg = current->thread_group;

    if (either_copyout(1, rgid_addr, &tg->gid, sizeof(uint32)) < 0)
        return (uint64)-EFAULT;
    if (either_copyout(1, egid_addr, &tg->egid, sizeof(uint32)) < 0)
        return (uint64)-EFAULT;
    if (either_copyout(1, sgid_addr, &tg->sgid, sizeof(uint32)) < 0)
        return (uint64)-EFAULT;
    return 0;
}

uint64 sys_getgroups(void) {
    int size;
    uint64 list_addr;
    argint(0, &size);
    argaddr(1, &list_addr);
    struct thread_group *tg = current->thread_group;

    if (size == 0)
        return (uint64)tg->ngroups;
    if (size < tg->ngroups)
        return (uint64)-EINVAL;
    if (tg->ngroups > 0) {
        if (either_copyout(1, list_addr, tg->groups,
                           sizeof(uint32) * tg->ngroups) < 0)
            return (uint64)-EFAULT;
    }
    return (uint64)tg->ngroups;
}

uint64 sys_setgroups(void) {
    int size;
    uint64 list_addr;
    argint(0, &size);
    argaddr(1, &list_addr);
    if (current->thread_group->euid != 0)
        return (uint64)-EPERM;
    if (size < 0 || size > NGROUPS_MAX)
        return (uint64)-EINVAL;
    struct thread_group *tg = current->thread_group;
    if (size > 0) {
        if (either_copyin(tg->groups, 1, list_addr,
                          sizeof(uint32) * size) < 0)
            return (uint64)-EFAULT;
    }
    tg->ngroups = size;
    return 0;
}

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

// vfork() — implemented as fork + CLONE_VFORK (parent blocks until
// child execs/exits).  We do NOT use CLONE_VM because musl/dash cannot
// safely operate in a shared-VM child: musl's internal locks, dash's
// longjmp-based error handling, and the shared stack all cause corruption.
// POSIX explicitly allows vfork to behave identically to fork.
uint64 sys_vfork(void) {
    struct clone_args args = {
        .flags = CLONE_VFORK,
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
    if (uargs < 0x100000) {
        // Linux-style clone(flags, stack, ...) where a0 is a flags bitmask.
        // This covers:
        //   - _Fork(): clone(SIGCHLD, 0)             — uargs = 17
        //   - vfork(): clone(CLONE_VM|CLONE_VFORK|SIGCHLD, sp) — uargs = 0x4111
        // Valid clone_args pointers are always > 0x100000 (user heap/stack).
        uint64 stack;
        argaddr(1, &stack);

        args.flags = uargs & ~0xFF;  // strip exit signal from flags
        args.esignal = uargs & 0xFF; // low 8 bits = exit signal
        args.stack = 0;              // 0 = inherit parent's stack (fork)
        args.stack_size = 0;

        // If CLONE_VM is set (vfork), pass the stack pointer
        if ((args.flags & CLONE_VM) && stack != 0) {
            // For vfork, the child shares parent's VM and stack.
            // Don't set args.stack — let child inherit parent's sp via
            // trapframe copy. The kernel's CLONE_VFORK handling will
            // block the parent until child execs/exits.
        }
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
    // dprintf("pid %d %s: clone(flags=0x%lx, stack=0x%lx, stack_size=0x%lx, entry=0x%lx, tls=0x%lx)\n",
    //        current->pid, current->name, args.flags, args.stack, args.stack_size,
    //        args.entry, args.tls);
    int ret = thread_clone(&args);
    // dprintf("pid %d %s: clone -> %d\n", current->pid, current->name, ret);
    return ret;
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
    uint64 remaining = sleep_ms_interruptible((uint64)n);
    if (remaining > 0 && signal_pending(current))
        return -EINTR;
    return 0;
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) { return get_jiffs(); }

/* Defined in kernel/daemons/sntpd.c \u2014 NTP\u2212RTC offset in nanoseconds */
extern volatile int64 sntp_offset_ns;
extern volatile int   sntp_synced;

uint64 sys_gettimeofday(void) {
    uint64 tv_addr;
    uint64 tz_addr;
    argaddr(0, &tv_addr);
    argaddr(1, &tz_addr);
    (void)tz_addr;

    if (tv_addr == 0) {
        return -EINVAL;
    }

    uint64 rtc = goldfish_rtc_read_ns();
    /* Apply NTP offset when available for more accurate wall-clock time */
    uint64 t = sntp_synced ? (uint64)((int64)rtc + sntp_offset_ns) : rtc;
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

    uint64 remaining_ms = sleep_ms_interruptible(ms);

    if (remaining_ms > 0 && signal_pending(current)) {
        // Write remaining time to user if address provided
        if (rem_addr != 0) {
            uint64 rem_ns = remaining_ms * 1000000ULL;
            struct __k_timespec rem = {
                .tv_sec = (int64)(rem_ns / 1000000000ULL),
                .tv_nsec = (int64)(rem_ns % 1000000000ULL),
            };
            // Best-effort write; ignore copyout failure per POSIX
            either_copyout(1, rem_addr, &rem, sizeof(rem));
        }
        return -EINTR;
    }

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
#if defined(CONFIG_ARCH_X86_64) || defined(__x86_64__)
    safestrcpy(u.machine, "x86_64", sizeof(u.machine));
#elif defined(CONFIG_ARCH_RISCV) || defined(__riscv)
    safestrcpy(u.machine, "riscv64", sizeof(u.machine));
#else
    safestrcpy(u.machine, "unknown", sizeof(u.machine));
#endif

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

    if (delta > 0)
        ACCT_ADD(current->thread_group, mm_brk_delta, delta);
    else if (delta < 0)
        ACCT_ADD(current->thread_group, mm_brk_delta, delta);
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
    {
        uint64 ns = goldfish_rtc_read_ns();
        ts.tv_sec = ns / 1000000000ULL;
        ts.tv_nsec = ns % 1000000000ULL;
        break;
    }
    case 1: // CLOCK_MONOTONIC
    {
        uint64 ms = get_jiffs();
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000ULL;
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
