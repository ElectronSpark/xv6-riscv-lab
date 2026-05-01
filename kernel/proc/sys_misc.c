/**
 * @file sys_misc.c
 * @brief Miscellaneous process-related syscall implementations.
 *
 * Implements: prctl, sysinfo, getrusage, getpriority, setpriority,
 * set_robust_list, clock_settime, sched_rr_get_interval, rt_sigqueueinfo,
 * clone3, mlock/munlock compatibility.
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "mm/vm.h"
#include "signal.h"
#include "syscall.h"
#include "errno.h"
#include "clone_flags.h"
#include "accounting.h"
#include "timer/timer.h"
#include "timer/goldfish_rtc.h"

/* Forward declarations for arg helpers */
void argint(int n, int *ip);
void argint64(int n, int64 *ip);
void argaddr(int n, uint64 *ip);
int argstr(int n, char *buf, int max);

/* ================================================================== */
/*  prctl                                                             */
/* ================================================================== */

#define PR_SET_NAME 15
#define PR_GET_NAME 16

uint64 sys_prctl(void) {
    int option;
    uint64 arg2, arg3, arg4, arg5;
    argint(0, &option);
    argaddr(1, &arg2);
    argaddr(2, &arg3);
    argaddr(3, &arg4);
    argaddr(4, &arg5);

    switch (option) {
    case PR_SET_NAME: {
        char name[16];
        if (either_copyin(name, 1, arg2, sizeof(name)) < 0)
            return (uint64)-EFAULT;
        name[15] = '\0';
        memmove(current->name, name, 16);
        return 0;
    }
    case PR_GET_NAME: {
        if (arg2 == 0)
            return (uint64)-EFAULT;
        char name[16];
        memmove(name, current->name, 16);
        name[15] = '\0';
        if (either_copyout(1, arg2, name, sizeof(name)) < 0)
            return (uint64)-EFAULT;
        return 0;
    }
    default:
        return (uint64)-EINVAL;
    }
}

/* ================================================================== */
/*  sysinfo                                                           */
/* ================================================================== */

/* Fixed-point shift used by avenrun[] (from sched.c) */
#define LOAD_FSHIFT 11
#define LOAD_FIXED_1 (1 << LOAD_FSHIFT)

/* Linux struct sysinfo layout (matches musl/glibc) */
struct k_sysinfo {
    int64  uptime;       /* Seconds since boot */
    uint64 loads[3];     /* 1, 5, and 15 minute load averages */
    uint64 totalram;     /* Total usable main memory size */
    uint64 freeram;      /* Available memory size */
    uint64 sharedram;    /* Amount of shared memory */
    uint64 bufferram;    /* Memory used by buffers */
    uint64 totalswap;    /* Total swap space size */
    uint64 freeswap;     /* Swap space still available */
    uint16 procs;        /* Number of current processes */
    uint16 __pad;
    uint32 __pad2;
    uint64 totalhigh;    /* Total high memory size */
    uint64 freehigh;     /* Available high memory size */
    uint32 mem_unit;     /* Memory unit size in bytes */
};

extern uint64 __physical_memory_start;
extern uint64 __physical_memory_end;

static void __sysinfo_count_proc(int tgid __attribute__((unused)), void *arg) {
    (*(int *)arg)++;
}

uint64 sys_sysinfo(void) {
    uint64 uaddr;
    argaddr(0, &uaddr);
    if (uaddr == 0)
        return (uint64)-EFAULT;

    struct k_sysinfo si;
    memset(&si, 0, sizeof(si));

    /* Uptime: get_jiffs() returns milliseconds; convert to seconds */
    si.uptime = (int64)(get_jiffs() / 1000);
    if (si.uptime == 0)
        si.uptime = 1;

    /* Load averages: kernel stores in LOAD_FSHIFT=11 fixed-point.
     * Linux sysinfo expects SI_LOAD_SHIFT=16 fixed-point. */
    uint64 loads[3];
    get_avenrun(loads);
    si.loads[0] = loads[0] << (16 - LOAD_FSHIFT);
    si.loads[1] = loads[1] << (16 - LOAD_FSHIFT);
    si.loads[2] = loads[2] << (16 - LOAD_FSHIFT);

    /* Memory info */
    si.totalram = __physical_memory_end - __physical_memory_start;
    si.freeram  = get_total_free_pages() * PAGE_SIZE;
    si.mem_unit = 1;

    /* Process count — scan the proctab for actual process count */
    {
        int nprocs = 0;
        proctab_for_each_tgid(__sysinfo_count_proc, &nprocs);
        si.procs = (uint16)nprocs;
    }

    if (either_copyout(1, uaddr, &si, sizeof(si)) < 0)
        return (uint64)-EFAULT;

    return 0;
}

/* ================================================================== */
/*  getrusage                                                         */
/* ================================================================== */

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

struct k_timeval {
    int64 tv_sec;
    int64 tv_usec;
};

struct k_rusage {
    struct k_timeval ru_utime;    /* user time used */
    struct k_timeval ru_stime;    /* system time used */
    int64 ru_maxrss;              /* max resident set size (KB) */
    int64 ru_ixrss;               /* integral shared text memory size */
    int64 ru_idrss;               /* integral unshared data size */
    int64 ru_isrss;               /* integral unshared stack size */
    int64 ru_minflt;              /* page reclaims (soft page faults) */
    int64 ru_majflt;              /* page faults (hard page faults) */
    int64 ru_nswap;               /* swaps */
    int64 ru_inblock;             /* block input operations */
    int64 ru_oublock;             /* block output operations */
    int64 ru_msgsnd;              /* messages sent */
    int64 ru_msgrcv;              /* messages received */
    int64 ru_nsignals;            /* signals received */
    int64 ru_nvcsw;               /* voluntary context switches */
    int64 ru_nivcsw;              /* involuntary context switches */
};

uint64 sys_getrusage(void) {
    int who;
    uint64 uaddr;
    argint(0, &who);
    argaddr(1, &uaddr);

    if (uaddr == 0)
        return (uint64)-EFAULT;

    struct k_rusage ru;
    memset(&ru, 0, sizeof(ru));

    if (who == RUSAGE_SELF || who == RUSAGE_THREAD) {
        struct thread_group *tg = current->thread_group;
        if (tg) {
            /* maxrss from accounting, convert bytes → KB */
            ru.ru_maxrss = (int64)(__atomic_load_n(&tg->acct.mm_peak_vm,
                                                    __ATOMIC_RELAXED) / 1024);
            /* Block I/O from accounting */
            ru.ru_inblock = (int64)__atomic_load_n(&tg->acct.bio_reads,
                                                    __ATOMIC_RELAXED);
            ru.ru_oublock = (int64)__atomic_load_n(&tg->acct.bio_writes,
                                                    __ATOMIC_RELAXED);
        }

        /* Approximate user time from scheduler's sum_exec_runtime */
        if (current->sched_entity) {
            uint64 runtime_ticks = current->sched_entity->sum_exec_runtime;
            /* Convert raw ticks to microseconds using timebase frequency */
            extern uint64 __timebase_frequency;
            if (__timebase_frequency > 0) {
                uint64 us = runtime_ticks * 1000000ULL / __timebase_frequency;
                ru.ru_utime.tv_sec  = (int64)(us / 1000000ULL);
                ru.ru_utime.tv_usec = (int64)(us % 1000000ULL);
            }
        }
    } else if (who == RUSAGE_CHILDREN) {
        /* Return zeros — we don't accumulate children's rusage */
    } else {
        return (uint64)-EINVAL;
    }

    if (either_copyout(1, uaddr, &ru, sizeof(ru)) < 0)
        return (uint64)-EFAULT;

    return 0;
}

/* ================================================================== */
/*  getpriority / setpriority                                         */
/* ================================================================== */

#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

/*
 * Linux getpriority returns 20 - nice, so range [1, 40].
 * The kernel scheduler uses priority levels 0-63 where
 * DEFAULT_MAJOR_PRIORITY = 17.  We map nice ∈ [-20,19] to
 * priority ∈ [0,39] via: priority = nice + 20.
 * nice = priority - 20.
 *
 * Linux convention: getpriority returns 20 - nice (always > 0 to
 * distinguish success from error).  Userspace must compute
 * nice = 20 - retval.
 */
uint64 sys_getpriority(void) {
    int which, who;
    argint(0, &which);
    argint(1, &who);

    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER)
        return (uint64)-EINVAL;

    struct thread *target = NULL;

    if (which == PRIO_PROCESS) {
        if (who == 0) {
            target = current;
        } else {
            struct thread *pp = NULL;
            if (get_pid_thread(who, &pp) != 0 || pp == NULL)
                return (uint64)-ESRCH;
            target = pp;
        }
    } else {
        /* PRIO_PGRP and PRIO_USER: fall back to self */
        target = current;
    }

    int nice = 0;
    if (target->sched_entity) {
        int prio = target->sched_entity->priority;
        nice = prio - 20;
        if (nice < -20) nice = -20;
        if (nice > 19)  nice = 19;
    }

    return (uint64)(20 - nice);
}

uint64 sys_setpriority(void) {
    int which, who, niceval;
    argint(0, &which);
    argint(1, &who);
    argint(2, &niceval);

    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER)
        return (uint64)-EINVAL;

    /* Clamp nice to [-20, 19] */
    if (niceval < -20) niceval = -20;
    if (niceval > 19)  niceval = 19;

    /* Only allow non-root to increase nice (lower priority) */
    if (niceval < 0 && current->thread_group->euid != 0)
        return (uint64)-EACCES;

    struct thread *target = NULL;

    if (which == PRIO_PROCESS) {
        if (who == 0) {
            target = current;
        } else {
            struct thread *pp = NULL;
            if (get_pid_thread(who, &pp) != 0 || pp == NULL)
                return (uint64)-ESRCH;
            target = pp;
        }
    } else {
        /* PRIO_PGRP and PRIO_USER: fall back to self */
        target = current;
    }

    if (target->sched_entity) {
        struct sched_attr attr;
        sched_attr_init(&attr);
        attr.priority = niceval + 20;
        sched_setattr(target->sched_entity, &attr);
    }

    return 0;
}

/* ================================================================== */
/*  set_robust_list                                                   */
/* ================================================================== */

uint64 sys_set_robust_list(void) {
    uint64 head;
    uint64 len;
    argaddr(0, &head);
    argaddr(1, &len);

    /* Linux requires len == 24 (sizeof(struct robust_list_head)) */
    if (len != 24)
        return (uint64)-EINVAL;

    current->robust_list_head = head;
    current->robust_list_len  = len;
    return 0;
}

/* ================================================================== */
/*  clock_settime                                                     */
/* ================================================================== */

uint64 sys_clock_settime(void) {
    /* xv6 does not support setting the clock.
     * Return -EPERM (permission denied) as an unprivileged stub. */
    return (uint64)-EPERM;
}

/* ================================================================== */
/*  sched_rr_get_interval                                             */
/* ================================================================== */

struct __k_timespec {
    int64 tv_sec;
    int64 tv_nsec;
};

uint64 sys_sched_rr_get_interval(void) {
    int pid;
    uint64 tp_addr;
    argint(0, &pid);
    argaddr(1, &tp_addr);

    if (tp_addr == 0)
        return (uint64)-EFAULT;

    /* Return a fixed 100ms time slice */
    struct __k_timespec ts = {
        .tv_sec  = 0,
        .tv_nsec = 100000000LL,  /* 100 ms */
    };

    if (either_copyout(1, tp_addr, &ts, sizeof(ts)) < 0)
        return (uint64)-EFAULT;

    return 0;
}

/* ================================================================== */
/*  sched_getaffinity / sched_setaffinity                             */
/* ================================================================== */

/*
 * sched_getaffinity(pid, cpusetsize, mask)
 * xv6 is single-CPU: return a mask with only bit 0 set.
 */
uint64 sys_sched_getaffinity(void) {
    int pid;
    int cpusetsize;
    uint64 mask_addr;
    argint(0, &pid);
    argint(1, &cpusetsize);
    argaddr(2, &mask_addr);

    if (mask_addr == 0)
        return (uint64)-EFAULT;
    if (cpusetsize <= 0)
        return (uint64)-EINVAL;

    /* Zero the entire mask, then set bit 0 (CPU 0) */
    uint8 buf[128];
    int len = cpusetsize;
    if (len > (int)sizeof(buf))
        len = (int)sizeof(buf);
    memset(buf, 0, len);
    buf[0] = 1;  /* CPU 0 */

    if (either_copyout(1, mask_addr, buf, len) < 0)
        return (uint64)-EFAULT;

    /* Linux returns the number of bytes written on success */
    return (uint64)len;
}

/*
 * sched_setaffinity(pid, cpusetsize, mask)
 * xv6 is single-CPU: accept but ignore the mask.
 */
uint64 sys_sched_setaffinity(void) {
    return 0;
}

/* ================================================================== */
/*  sched_yield                                                       */
/* ================================================================== */

uint64 sys_sched_yield(void) {
    scheduler_yield();
    return 0;
}

/* ================================================================== */
/*  sched_getscheduler / sched_setscheduler                           */
/* ================================================================== */

#define SCHED_OTHER 0

uint64 sys_sched_getscheduler(void) {
    return SCHED_OTHER;
}

uint64 sys_sched_setscheduler(void) {
    return 0;
}

/* ================================================================== */
/*  sched_get_priority_max / sched_get_priority_min                   */
/* ================================================================== */

uint64 sys_sched_get_priority_max(void) {
    return 0;
}

uint64 sys_sched_get_priority_min(void) {
    return 0;
}

/* ================================================================== */
/*  rt_sigqueueinfo                                                   */
/* ================================================================== */

uint64 sys_rt_sigqueueinfo(void) {
    int tgid, sig;
    uint64 uinfo_addr;
    argint(0, &tgid);
    argint(1, &sig);
    argaddr(2, &uinfo_addr);

    if (SIGBAD(sig))
        return (uint64)-EINVAL;

    if (uinfo_addr == 0)
        return (uint64)-EFAULT;

    /* Copy siginfo from user */
    siginfo_t uinfo;
    if (either_copyin(&uinfo, 1, uinfo_addr, sizeof(uinfo)) < 0)
        return (uint64)-EFAULT;

    /* Allocate a kernel siginfo and deliver */
    ksiginfo_t *ksi = ksiginfo_alloc();
    if (ksi == NULL)
        return (uint64)-EAGAIN;

    ksi->signo = sig;
    ksi->sender = current;
    ksi->info = uinfo;
    ksi->info.si_signo = sig;

    int ret = signal_send(tgid, ksi);
    if (ret < 0)
        ksiginfo_free(ksi);

    return (uint64)ret;
}

/* ================================================================== */
/*  clone3                                                            */
/* ================================================================== */

/*
 * Linux clone3 passes a pointer to struct clone_args + size.
 * We map a subset of fields to xv6's struct clone_args and call
 * thread_clone().
 */
struct linux_clone_args {
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

uint64 sys_clone3(void) {
    uint64 uargs_addr;
    uint64 size;
    argaddr(0, &uargs_addr);
    argaddr(1, &size);

    if (uargs_addr == 0)
        return (uint64)-EINVAL;

    /* Copy in the Linux clone_args (up to what we support) */
    struct linux_clone_args largs;
    memset(&largs, 0, sizeof(largs));

    uint64 copy_size = size;
    if (copy_size > sizeof(largs))
        copy_size = sizeof(largs);

    if (vm_copyin(current->vm, &largs, uargs_addr, copy_size) < 0)
        return (uint64)-EFAULT;

    /* Map to xv6 clone_args */
    struct clone_args args = {
        .flags      = largs.flags,
        .stack      = largs.stack,
        .stack_size = largs.stack_size,
        .entry      = 0,
        .esignal    = largs.exit_signal,
        .tls        = largs.tls,
        .ctid       = largs.child_tid,
        .ptid       = largs.parent_tid,
    };

    return (uint64)thread_clone(&args);
}

#define MLOCK_ONFAULT 0x1
#define MCL_CURRENT  0x1
#define MCL_FUTURE   0x2
#define MCL_ONFAULT  0x4

static uint64 sys_mlock_range(uint64 addr, uint64 length, int flags)
{
    if (flags & ~MLOCK_ONFAULT)
        return (uint64)-EINVAL;

    if (length == 0)
        return 0;
    if (addr + length < addr)
        return (uint64)-EINVAL;

    /*
     * xv6 does not swap pages out, so the Linux memory-locking contract is
     * already satisfied for resident user mappings.  Keep the syscall as a
     * validated no-op so libraries using secure arenas can run unmodified.
     */
    if (addr >= MAXVA || addr + length > MAXVA)
        return (uint64)-ENOMEM;

    return 0;
}

uint64 sys_mlock2(void) {
    uint64 addr;
    uint64 length;
    int flags;

    argaddr(0, &addr);
    argaddr(1, &length);
    argint(2, &flags);

    return sys_mlock_range(addr, length, flags);
}

uint64 sys_mlockall(void) {
    int flags;
    argint(0, &flags);

    if (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT))
        return (uint64)-EINVAL;

    return 0;
}

uint64 sys_munlock(void) {
    uint64 addr;
    uint64 length;

    argaddr(0, &addr);
    argaddr(1, &length);

    return sys_mlock_range(addr, length, 0);
}

uint64 sys_munlockall(void) {
    return 0;
}

/* ================================================================== */
/*  membarrier — stub (returns 0 for CMD_QUERY, -ENOSYS otherwise)    */
/* ================================================================== */

uint64 sys_membarrier(void) {
    int cmd;
    argint(0, &cmd);

    /* CMD 0 = MEMBARRIER_CMD_QUERY: return bitmask of supported commands.
     * We support nothing, so return 0. */
    if (cmd == 0)
        return 0;

    return (uint64)-ENOSYS;
}
