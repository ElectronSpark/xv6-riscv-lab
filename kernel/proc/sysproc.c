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
#include "kstats.h"
#include "proc/pgroup.h"
#include "tty/session.h"
#include "timer/goldfish_rtc.h"
#include "list.h"

#define SYSCALL_PROFILE_BEGIN(call_ctr)                                     \
    uint64 __sys_start = r_time();                                          \
    __atomic_add_fetch(&(call_ctr), 1, __ATOMIC_RELAXED)

#define SYSCALL_PROFILE_RETURN(ret_expr, tick_ctr)                          \
    do {                                                                    \
        uint64 __sys_ret = (uint64)(ret_expr);                              \
        __atomic_add_fetch(&(tick_ctr), r_time() - __sys_start,             \
                           __ATOMIC_RELAXED);                               \
        return __sys_ret;                                                   \
    } while (0)

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

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define CLOCK_REALTIME_ALARM     8
#define CLOCK_BOOTTIME_ALARM     9
#define CLOCK_SGI_CYCLE         10
#define CLOCK_TAI               11

#define TIMER_ABSTIME 1

/* Defined in kernel/daemons/sntpd.c — NTP−RTC offset in nanoseconds */
extern volatile int64 sntp_offset_ns;
extern volatile int   sntp_synced;

static uint64 raw_ticks_to_ns(uint64 ticks)
{
    uint64 freq = __timebase_frequency;
    if (freq == 0)
        return ticks;
    return (ticks / freq) * NS_PER_SEC +
           ((ticks % freq) * NS_PER_SEC) / freq;
}

static uint64 sched_entity_runtime_ticks(struct sched_entity *se)
{
    if (se == NULL)
        return 0;

    uint64 runtime = __atomic_load_n(&se->sum_exec_runtime,
                                     __ATOMIC_RELAXED);
    if (__atomic_load_n(&se->on_cpu, __ATOMIC_ACQUIRE)) {
        uint64 start = __atomic_load_n(&se->exec_start, __ATOMIC_RELAXED);
        uint64 now = r_time();
        if (now > start)
            runtime += now - start;
    }
    return runtime;
}

static uint64 thread_group_runtime_ticks(struct thread_group *tg)
{
    if (tg == NULL)
        return 0;

    uint64 runtime = 0;
    pid_rlock();
    struct thread *t;
    struct thread *tmp;
    list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
        runtime += sched_entity_runtime_ticks(t->sched_entity);
    }
    pid_runlock();
    return runtime;
}

static void ns_to_timespec(uint64 ns, struct __k_timespec *ts)
{
    ts->tv_sec = (int64)(ns / NS_PER_SEC);
    ts->tv_nsec = (int64)(ns % NS_PER_SEC);
}

static int timespec_to_ns(const struct __k_timespec *ts, uint64 *ns)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= NS_PER_SEC)
        return -EINVAL;
    if ((uint64)ts->tv_sec > UINT64_MAX / NS_PER_SEC)
        return -EINVAL;
    uint64 base = (uint64)ts->tv_sec * NS_PER_SEC;
    if (base > UINT64_MAX - (uint64)ts->tv_nsec)
        return -EINVAL;
    *ns = base + (uint64)ts->tv_nsec;
    return 0;
}

static int clock_get_ns(int clockid, uint64 *ns)
{
    switch (clockid) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_REALTIME_ALARM:
    case CLOCK_TAI: {
        uint64 rtc = goldfish_rtc_read_ns();
        *ns = sntp_synced ? (uint64)((int64)rtc + sntp_offset_ns) : rtc;
        return 0;
    }
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
    case CLOCK_BOOTTIME_ALARM:
        *ns = raw_ticks_to_ns(r_time());
        return 0;
    case CLOCK_PROCESS_CPUTIME_ID:
        *ns = raw_ticks_to_ns(thread_group_runtime_ticks(current->thread_group));
        return 0;
    case CLOCK_THREAD_CPUTIME_ID:
        *ns = raw_ticks_to_ns(
            sched_entity_runtime_ticks(current->sched_entity));
        return 0;
    default:
        return -EINVAL;
    }
}

static bool clock_can_sleep(int clockid)
{
    return clockid == CLOCK_REALTIME ||
           clockid == CLOCK_MONOTONIC ||
           clockid == CLOCK_BOOTTIME ||
           clockid == CLOCK_REALTIME_ALARM ||
           clockid == CLOCK_BOOTTIME_ALARM ||
           clockid == CLOCK_TAI;
}

uint64 sys_exit(void) {
    int n;
    argint(0, &n);
    /*
     * Keep raw exit useful for pthread workers, but do not leave a GUI or
     * server process half-alive when its thread-group leader exits via the
     * legacy xv6 syscall path instead of exit_group.
     */
    if (thread_is_group_leader(current) && current->thread_group != NULL &&
        __atomic_load_n(&current->thread_group->live_threads,
                        __ATOMIC_RELAXED) > 1) {
        thread_group_exit(current, n);
    }
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

// vfork() uses Linux semantics: the child shares the caller's VM and the
// parent remains blocked until the child execs or exits.
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
    uint64 linux_clone_mask =
        0xffULL | CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
        CLONE_PIDFD | CLONE_PTRACE | CLONE_VFORK | CLONE_PARENT |
        CLONE_THREAD | CLONE_NEWNS | CLONE_SYSVSEM | CLONE_SETTLS |
        CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED |
        CLONE_UNTRACED | CLONE_CHILD_SETTID | CLONE_NEWCGROUP |
        CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
        CLONE_NEWNET | CLONE_IO | CLONE_CLEAR_SIGHAND |
        CLONE_INTO_CGROUP | CLONE_PID | CLONE_SYSTEM | CLONE_SIGSTOPPED;

    if ((uargs & ~linux_clone_mask) == 0) {
        // Linux-style clone(flags, stack, ptid, ctid, tls).  musl's
        // pthread_create uses CLONE_CHILD_CLEARTID and friends, so a numeric
        // threshold is not a safe way to distinguish this ABI from the legacy
        // xv6 clone_args pointer ABI.
        uint64 stack, ptid, ctid, tls;
        argaddr(1, &stack);
        argaddr(2, &ptid);
        argaddr(3, &ctid);
        argaddr(4, &tls);

        args.flags = uargs & ~0xFF;  // strip exit signal from flags
        args.esignal = uargs & 0xFF; // low 8 bits = exit signal
        args.stack = stack;
        args.stack_size = 0;
        args.entry = 0;
        args.ptid = ptid;
        args.ctid = ctid;
        args.tls = tls;
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
    int ret = thread_clone(&args);
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
    SYSCALL_PROFILE_BEGIN(g_sys_gettimeofday_calls);
    uint64 tv_addr;
    uint64 tz_addr;
    argaddr(0, &tv_addr);
    argaddr(1, &tz_addr);
    (void)tz_addr;

    if (tv_addr == 0) {
        SYSCALL_PROFILE_RETURN(-EINVAL, g_sys_gettimeofday_ticks);
    }

    uint64 rtc = goldfish_rtc_read_ns();
    /* Apply NTP offset when available for more accurate wall-clock time */
    uint64 t = sntp_synced ? (uint64)((int64)rtc + sntp_offset_ns) : rtc;
    struct __k_timeval tv = {
        .tv_sec = t / NS_PER_SEC,
        .tv_usec = (int64)((t % NS_PER_SEC) / NS_PER_US),
    };

    if (either_copyout(1, tv_addr, &tv, sizeof(tv)) < 0) {
        SYSCALL_PROFILE_RETURN(-EFAULT, g_sys_gettimeofday_ticks);
    }
    SYSCALL_PROFILE_RETURN(0, g_sys_gettimeofday_ticks);
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

uint64 sys_clock_nanosleep(void) {
    int clockid, flags;
    uint64 req_addr, rem_addr;
    argint(0, &clockid);
    argint(1, &flags);
    argaddr(2, &req_addr);
    argaddr(3, &rem_addr);

    if (req_addr == 0)
        return -EINVAL;
    if (flags & ~TIMER_ABSTIME)
        return -EINVAL;
    if (!clock_can_sleep(clockid))
        return -EINVAL;

    struct __k_timespec req = {0};
    if (either_copyin(&req, 1, req_addr, sizeof(req)) < 0)
        return -EFAULT;

    uint64 target_ns;
    int ret = timespec_to_ns(&req, &target_ns);
    if (ret != 0)
        return ret;

    uint64 sleep_ns = target_ns;
    if (flags & TIMER_ABSTIME) {
        uint64 now_ns;
        ret = clock_get_ns(clockid, &now_ns);
        if (ret != 0)
            return ret;
        if (target_ns <= now_ns)
            return 0;
        sleep_ns = target_ns - now_ns;
    }

    uint64 ms = (sleep_ns + NS_PER_MS - 1) / NS_PER_MS;
    if (sleep_ns > 0 && ms == 0)
        ms = 1;

    uint64 remaining_ms = sleep_ms_interruptible(ms);
    if (remaining_ms > 0 && signal_pending(current)) {
        if (!(flags & TIMER_ABSTIME) && rem_addr != 0) {
            struct __k_timespec rem;
            ns_to_timespec(remaining_ms * NS_PER_MS, &rem);
            either_copyout(1, rem_addr, &rem, sizeof(rem));
        }
        return -EINTR;
    }

    if (!(flags & TIMER_ABSTIME) && rem_addr != 0) {
        struct __k_timespec rem = {0};
        if (either_copyout(1, rem_addr, &rem, sizeof(rem)) < 0)
            return -EFAULT;
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
    SYSCALL_PROFILE_BEGIN(g_sys_getrandom_calls);
    uint64 ubuf;
    int len;

    argaddr(0, &ubuf);
    argint(1, &len);

    if (len < 0) {
        SYSCALL_PROFILE_RETURN(-EINVAL, g_sys_getrandom_ticks);
    }
    if (len == 0) {
        SYSCALL_PROFILE_RETURN(0, g_sys_getrandom_ticks);
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
            SYSCALL_PROFILE_RETURN(done ? done : -EFAULT,
                                   g_sys_getrandom_ticks);
        }
        done += chunk;
    }

    SYSCALL_PROFILE_RETURN(done, g_sys_getrandom_ticks);
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
    SYSCALL_PROFILE_BEGIN(g_sys_brk_calls);
    uint64 addr;
    argaddr(0, &addr);

    vm_t *vm = current->vm;
    vma_t *heap = vm->heap;
    if (heap == NULL)
        SYSCALL_PROFILE_RETURN(-ENOMEM, g_sys_brk_ticks);

    uint64 cur_brk = heap->start + vm->heap_size;

    if (addr == 0) {
        SYSCALL_PROFILE_RETURN(cur_brk, g_sys_brk_ticks);
    }

    if (addr < heap->start)
        SYSCALL_PROFILE_RETURN(cur_brk, g_sys_brk_ticks); // Cannot shrink below heap start

    int64 delta = (int64)(addr - cur_brk);
    if (delta == 0)
        SYSCALL_PROFILE_RETURN(cur_brk, g_sys_brk_ticks);

    if (vm_growheap(vm, delta) < 0) {
        SYSCALL_PROFILE_RETURN(cur_brk, g_sys_brk_ticks); // Return old break on failure
    }

    if (delta > 0)
        ACCT_ADD(current->thread_group, mm_brk_delta, delta);
    else if (delta < 0)
        ACCT_ADD(current->thread_group, mm_brk_delta, delta);
    SYSCALL_PROFILE_RETURN(heap->start + vm->heap_size, g_sys_brk_ticks);
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
    SYSCALL_PROFILE_BEGIN(g_sys_clock_gettime_calls);
    int clockid;
    uint64 tp_addr;
    argint(0, &clockid);
    argaddr(1, &tp_addr);

    if (tp_addr == 0)
        SYSCALL_PROFILE_RETURN(-EINVAL, g_sys_clock_gettime_ticks);

    uint64 ns;
    int ret = clock_get_ns(clockid, &ns);
    if (ret != 0)
        SYSCALL_PROFILE_RETURN(ret, g_sys_clock_gettime_ticks);

    struct __k_timespec ts = {0};
    ns_to_timespec(ns, &ts);

    if (either_copyout(1, tp_addr, &ts, sizeof(ts)) < 0)
        SYSCALL_PROFILE_RETURN(-EFAULT, g_sys_clock_gettime_ticks);

    SYSCALL_PROFILE_RETURN(0, g_sys_clock_gettime_ticks);
}

/*
 * clock_getres(clockid, res) — get resolution of specified clock.
 */
uint64 sys_clock_getres(void) {
    int clockid;
    uint64 res_addr;
    argint(0, &clockid);
    argaddr(1, &res_addr);

    switch (clockid) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
    case CLOCK_REALTIME_ALARM:
    case CLOCK_BOOTTIME_ALARM:
    case CLOCK_TAI:
        break;
    default:
        return -EINVAL;
    }

    if (res_addr != 0) {
        struct __k_timespec res = {.tv_sec = 0, .tv_nsec = 1000};
        if (either_copyout(1, res_addr, &res, sizeof(res)) < 0)
            return -EFAULT;
    }
    return 0;
}
