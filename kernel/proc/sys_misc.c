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
#include "proc/pgroup.h"

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

struct k_tms {
    int64 tms_utime;
    int64 tms_stime;
    int64 tms_cutime;
    int64 tms_cstime;
};

uint64 sys_times(void) {
    uint64 uaddr;
    argaddr(0, &uaddr);

    if (uaddr != 0) {
        struct k_tms tms;
        memset(&tms, 0, sizeof(tms));
        if (current->sched_entity != NULL)
            tms.tms_utime = (int64)current->sched_entity->sum_exec_runtime;
        if (either_copyout(1, uaddr, &tms, sizeof(tms)) < 0)
            return (uint64)-EFAULT;
    }

    return get_jiffs();
}

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

static int nice_to_eevdf_priority(int nice)
{
    if (nice < -20)
        nice = -20;
    if (nice > 19)
        nice = 19;
    return EEVDF_PRIORITY_START + nice + 20;
}

static int priority_to_nice(int priority)
{
    if (!IS_EEVDF_PRIORITY(priority))
        return 0;
    return priority - EEVDF_PRIORITY_START - 20;
}

/*
 * Linux getpriority returns 20 - nice, so range [1, 40].
 * Normal SCHED_OTHER tasks live in the EEVDF priority range:
 * nice -20 maps to EEVDF_PRIORITY_START, nice 0 maps to
 * DEFAULT_PRIORITY, and nice 19 maps to EEVDF_PRIORITY_LIMIT - 1.
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
        nice = priority_to_nice(target->sched_entity->priority);
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
        attr.priority = nice_to_eevdf_priority(niceval);
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

uint64 sys_get_robust_list(void) {
    int pid;
    uint64 head_ptr;
    uint64 len_ptr;

    argint(0, &pid);
    argaddr(1, &head_ptr);
    argaddr(2, &len_ptr);

    /*
     * xv6 does not currently expose ptrace-style cross-task inspection here.
     * Linux allows pid 0 to mean the calling thread, which is what libcs use.
     */
    if (pid != 0 && pid != current->pid)
        return (uint64)-EPERM;

    uint64 head = current->robust_list_head;
    uint64 len = current->robust_list_len;
    if (vm_copyout(current->vm, head_ptr, &head, sizeof(head)) < 0 ||
        vm_copyout(current->vm, len_ptr, &len, sizeof(len)) < 0)
        return (uint64)-EFAULT;

    return 0;
}

/* ================================================================== */
/*  getcpu / rseq / capabilities                                      */
/* ================================================================== */

uint64 sys_getcpu(void) {
    uint64 cpu_addr;
    uint64 node_addr;
    argaddr(0, &cpu_addr);
    argaddr(1, &node_addr);

    uint32 cpu = (uint32)cpuid();
    uint32 node = 0;
    if (cpu_addr != 0 &&
        either_copyout(1, cpu_addr, &cpu, sizeof(cpu)) < 0)
        return (uint64)-EFAULT;
    if (node_addr != 0 &&
        either_copyout(1, node_addr, &node, sizeof(node)) < 0)
        return (uint64)-EFAULT;
    return 0;
}

#define RSEQ_FLAG_UNREGISTER 1
#define RSEQ_MIN_SIZE 32
#define RSEQ_ALIGNMENT 32
#define RSEQ_CPU_ID_UNINITIALIZED 0xffffffffU

struct linux_rseq_area {
    uint32 cpu_id_start;
    uint32 cpu_id;
    uint64 rseq_cs;
    uint32 flags;
};

static int rseq_write_state(uint64 rseq_addr, uint32 cpu_id)
{
    struct linux_rseq_area area;

    memset(&area, 0, sizeof(area));
    area.cpu_id_start = cpu_id;
    area.cpu_id = cpu_id;
    return either_copyout(1, rseq_addr, &area, sizeof(area));
}

uint64 sys_rseq(void) {
    uint64 rseq_addr;
    uint32 rseq_len;
    int flags;
    uint32 signature;

    argaddr(0, &rseq_addr);
    argint(1, (int *)&rseq_len);
    argint(2, &flags);
    argint(3, (int *)&signature);

    if (flags & ~RSEQ_FLAG_UNREGISTER)
        return (uint64)-EINVAL;

    if (flags & RSEQ_FLAG_UNREGISTER) {
        if (current->rseq_addr == 0 || current->rseq_addr != rseq_addr ||
            current->rseq_len != rseq_len ||
            current->rseq_signature != signature)
            return (uint64)-EINVAL;
        if (rseq_write_state(rseq_addr, RSEQ_CPU_ID_UNINITIALIZED) < 0)
            return (uint64)-EFAULT;
        current->rseq_addr = 0;
        current->rseq_len = 0;
        current->rseq_signature = 0;
        return 0;
    }

    if (rseq_addr == 0 || rseq_len != RSEQ_MIN_SIZE ||
        (rseq_addr & (RSEQ_ALIGNMENT - 1)) != 0)
        return (uint64)-EINVAL;
    if (current->rseq_addr != 0)
        return (uint64)-EBUSY;
    if (rseq_write_state(rseq_addr, (uint32)cpuid()) < 0)
        return (uint64)-EFAULT;

    current->rseq_addr = rseq_addr;
    current->rseq_len = rseq_len;
    current->rseq_signature = signature;
    return 0;
}

#define _LINUX_CAPABILITY_VERSION_1 0x19980330
#define _LINUX_CAPABILITY_VERSION_2 0x20071026
#define _LINUX_CAPABILITY_VERSION_3 0x20080522

struct linux_cap_user_header {
    uint32 version;
    int pid;
};

struct linux_cap_user_data {
    uint32 effective;
    uint32 permitted;
    uint32 inheritable;
};

static int cap_words_for_version(uint32 version)
{
    switch (version) {
    case _LINUX_CAPABILITY_VERSION_1:
        return 1;
    case _LINUX_CAPABILITY_VERSION_2:
    case _LINUX_CAPABILITY_VERSION_3:
        return 2;
    default:
        return -EINVAL;
    }
}

uint64 sys_capget(void) {
    uint64 hdr_addr;
    uint64 data_addr;
    argaddr(0, &hdr_addr);
    argaddr(1, &data_addr);

    if (hdr_addr == 0)
        return (uint64)-EFAULT;

    struct linux_cap_user_header hdr;
    if (either_copyin(&hdr, 1, hdr_addr, sizeof(hdr)) < 0)
        return (uint64)-EFAULT;

    int words = cap_words_for_version(hdr.version);
    if (words < 0) {
        hdr.version = _LINUX_CAPABILITY_VERSION_3;
        either_copyout(1, hdr_addr, &hdr, sizeof(hdr));
        return (uint64)-EINVAL;
    }
    if (hdr.pid < 0)
        return (uint64)-EINVAL;
    if (hdr.pid != 0 && hdr.pid != current->pid && hdr.pid != thread_tgid(current))
        return (uint64)-ESRCH;
    if (data_addr == 0)
        return 0;

    struct linux_cap_user_data data[2];
    memset(data, 0, sizeof(data));
    if (either_copyout(1, data_addr, data,
                       (size_t)words * sizeof(data[0])) < 0)
        return (uint64)-EFAULT;
    return 0;
}

uint64 sys_capset(void) {
    uint64 hdr_addr;
    uint64 data_addr;
    argaddr(0, &hdr_addr);
    argaddr(1, &data_addr);

    if (hdr_addr == 0 || data_addr == 0)
        return (uint64)-EFAULT;

    struct linux_cap_user_header hdr;
    if (either_copyin(&hdr, 1, hdr_addr, sizeof(hdr)) < 0)
        return (uint64)-EFAULT;
    int words = cap_words_for_version(hdr.version);
    if (words < 0)
        return (uint64)-EINVAL;
    if (hdr.pid < 0)
        return (uint64)-EINVAL;
    if (hdr.pid != 0 && hdr.pid != current->pid && hdr.pid != thread_tgid(current))
        return (uint64)-ESRCH;

    struct linux_cap_user_data data[2];
    memset(data, 0, sizeof(data));
    if (either_copyin(data, 1, data_addr,
                      (size_t)words * sizeof(data[0])) < 0)
        return (uint64)-EFAULT;
    for (int i = 0; i < words; i++) {
        if (data[i].effective || data[i].permitted || data[i].inheritable)
            return (uint64)-EPERM;
    }
    return 0;
}

struct linux_sched_param {
    int sched_priority;
};

#define SCHED_OTHER 0
#define LINUX_SCHED_ATTR_SIZE_VER0 48
#define LINUX_SCHED_ATTR_SIZE      56

struct linux_sched_attr {
    uint32 size;
    uint32 sched_policy;
    uint64 sched_flags;
    int32 sched_nice;
    uint32 sched_priority;
    uint64 sched_runtime;
    uint64 sched_deadline;
    uint64 sched_period;
    uint32 sched_util_min;
    uint32 sched_util_max;
};

uint64 sys_sched_getparam(void) {
    int pid;
    uint64 param_addr;
    argint(0, &pid);
    argaddr(1, &param_addr);
    if (param_addr == 0)
        return (uint64)-EFAULT;
    if (pid < 0)
        return (uint64)-EINVAL;

    struct linux_sched_param param = { .sched_priority = 0 };
    if (either_copyout(1, param_addr, &param, sizeof(param)) < 0)
        return (uint64)-EFAULT;
    return 0;
}

uint64 sys_sched_setparam(void) {
    int pid;
    uint64 param_addr;
    argint(0, &pid);
    argaddr(1, &param_addr);
    if (pid < 0)
        return (uint64)-EINVAL;
    if (param_addr == 0)
        return (uint64)-EFAULT;

    struct linux_sched_param param;
    if (either_copyin(&param, 1, param_addr, sizeof(param)) < 0)
        return (uint64)-EFAULT;
    if (param.sched_priority != 0)
        return (uint64)-EINVAL;
    return 0;
}

uint64 sys_sched_getattr(void) {
    int pid;
    uint64 attr_addr;
    int size;
    uint flags;
    argint(0, &pid);
    argaddr(1, &attr_addr);
    argint(2, &size);
    argint(3, (int *)&flags);

    if (pid < 0)
        return (uint64)-EINVAL;
    if (attr_addr == 0)
        return (uint64)-EFAULT;
    if (size < LINUX_SCHED_ATTR_SIZE_VER0)
        return (uint64)-EINVAL;
    if (flags != 0)
        return (uint64)-EINVAL;

    struct linux_sched_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.sched_policy = SCHED_OTHER;
    attr.sched_nice = 0;
    attr.sched_priority = 0;

    size_t copy_size = (size_t)size;
    if (copy_size > sizeof(attr))
        copy_size = sizeof(attr);
    if (either_copyout(1, attr_addr, &attr, copy_size) < 0)
        return (uint64)-EFAULT;
    return 0;
}

uint64 sys_sched_setattr(void) {
    int pid;
    uint64 attr_addr;
    uint flags;
    argint(0, &pid);
    argaddr(1, &attr_addr);
    argint(2, (int *)&flags);

    if (pid < 0)
        return (uint64)-EINVAL;
    if (attr_addr == 0)
        return (uint64)-EFAULT;
    if (flags != 0)
        return (uint64)-EINVAL;

    uint32 user_size = 0;
    if (either_copyin(&user_size, 1, attr_addr, sizeof(user_size)) < 0)
        return (uint64)-EFAULT;
    if (user_size < LINUX_SCHED_ATTR_SIZE_VER0)
        return (uint64)-EINVAL;

    struct linux_sched_attr attr;
    memset(&attr, 0, sizeof(attr));
    size_t copy_size = user_size;
    if (copy_size > sizeof(attr))
        copy_size = sizeof(attr);
    if (either_copyin(&attr, 1, attr_addr, copy_size) < 0)
        return (uint64)-EFAULT;

    if (attr.sched_policy != SCHED_OTHER || attr.sched_flags != 0 ||
        attr.sched_nice != 0 || attr.sched_priority != 0 ||
        attr.sched_runtime != 0 || attr.sched_deadline != 0 ||
        attr.sched_period != 0)
        return (uint64)-EINVAL;
    return 0;
}

#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_WHO_PGRP    2
#define IOPRIO_WHO_USER    3
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_CLASS_MASK  0x7
#define IOPRIO_PRIO_MASK   0xff

uint64 sys_ioprio_get(void) {
    int which;
    int who;
    argint(0, &which);
    argint(1, &who);

    if (which < IOPRIO_WHO_PROCESS || which > IOPRIO_WHO_USER)
        return (uint64)-EINVAL;
    if (who < 0)
        return (uint64)-EINVAL;
    return 0;
}

uint64 sys_ioprio_set(void) {
    int which;
    int who;
    int ioprio;
    argint(0, &which);
    argint(1, &who);
    argint(2, &ioprio);

    if (which < IOPRIO_WHO_PROCESS || which > IOPRIO_WHO_USER)
        return (uint64)-EINVAL;
    if (who < 0)
        return (uint64)-EINVAL;

    int cls = (ioprio >> IOPRIO_CLASS_SHIFT) & IOPRIO_CLASS_MASK;
    int data = ioprio & IOPRIO_PRIO_MASK;
    if (ioprio < 0 || cls > 3 || data > 7)
        return (uint64)-EINVAL;
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
    struct sched_entity *se = current ? current->sched_entity : NULL;

    if (se && se->rq && se->sched_class && se->sched_class->yield_task) {
        int intr = rq_lock_current_irqsave();

        rq_yield_task();
        rq_unlock_current_irqrestore(intr);
    }
    scheduler_yield();
    return 0;
}

/* ================================================================== */
/*  sched_getscheduler / sched_setscheduler                           */
/* ================================================================== */

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
    /*
     * glibc probes clone3() before falling back to clone().  Returning EINVAL
     * from a partial clone3 implementation makes pthread_create fail, while
     * ENOSYS matches older Linux kernels and preserves the working clone ABI.
     */
    return (uint64)-ENOSYS;
#if 0
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
#endif
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

uint64 sys_mlock(void) {
    uint64 addr;
    uint64 length;

    argaddr(0, &addr);
    argaddr(1, &length);

    return sys_mlock_range(addr, length, 0);
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
/*  membarrier                                                        */
/* ================================================================== */

#define MEMBARRIER_CMD_QUERY                     0
#define MEMBARRIER_CMD_GLOBAL                    (1 << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED          (1 << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED (1 << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED         (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED (1 << 4)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE (1 << 5)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE (1 << 6)

#define MEMBARRIER_SUPPORTED                                             \
    (MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED |           \
     MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED |                          \
     MEMBARRIER_CMD_PRIVATE_EXPEDITED |                                  \
     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED |                         \
     MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE |                        \
     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE)

uint64 sys_membarrier(void) {
    int cmd;
    int flags;
    argint(0, &cmd);
    argint(1, &flags);

    if (flags != 0)
        return (uint64)-EINVAL;

    if (cmd == MEMBARRIER_CMD_QUERY)
        return MEMBARRIER_SUPPORTED;

    if (cmd == MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED ||
        cmd == MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED ||
        cmd == MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE)
        return 0;

    if (cmd == MEMBARRIER_CMD_GLOBAL ||
        cmd == MEMBARRIER_CMD_GLOBAL_EXPEDITED ||
        cmd == MEMBARRIER_CMD_PRIVATE_EXPEDITED ||
        cmd == MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return 0;
    }

    return (uint64)-EINVAL;
}

/* ================================================================== */
/*  Linux optional/privileged compatibility syscalls                   */
/* ================================================================== */

#define LINUX_PERSONALITY_QUERY 0xffffffffUL

uint64 sys_personality(void)
{
    uint64 persona;
    argaddr(0, &persona);

    /*
     * Linux userland commonly probes the current personality with
     * personality(0xffffffff).  xv6 exposes the default Linux personality and
     * accepts setting that same value; unsupported emulation modes are rejected.
     */
    if (persona == LINUX_PERSONALITY_QUERY || persona == 0)
        return 0;
    return (uint64)-EINVAL;
}

static uint64 linux_name_setter_stub(void)
{
    uint64 name_addr;
    int len;
    argaddr(0, &name_addr);
    argint(1, &len);

    if (len < 0 || len > 64)
        return (uint64)-EINVAL;
    if (len > 0 && name_addr == 0)
        return (uint64)-EFAULT;

    char tmp[65];
    if (len > 0 && either_copyin(tmp, 1, name_addr, (uint64)len) < 0)
        return (uint64)-EFAULT;
    return 0;
}

uint64 sys_sethostname(void) { return linux_name_setter_stub(); }
uint64 sys_setdomainname(void) { return linux_name_setter_stub(); }

uint64 sys_get_thread_area(void)
{
    return (uint64)-EINVAL;
}

uint64 sys_set_thread_area(void)
{
    return (uint64)-EINVAL;
}

uint64 sys_restart_syscall(void)
{
    return (uint64)-EINTR;
}

#define LINUX_PRIVILEGED_STUB(name) \
    uint64 sys_##name(void) { return (uint64)-EPERM; }

#define LINUX_INVALID_STUB(name) \
    uint64 sys_##name(void) { return (uint64)-EINVAL; }

#define LINUX_NODEV_STUB(name) \
    uint64 sys_##name(void) { return (uint64)-ENODEV; }

LINUX_PRIVILEGED_STUB(ptrace)
LINUX_PRIVILEGED_STUB(syslog)
LINUX_INVALID_STUB(uselib)
LINUX_INVALID_STUB(ustat)
LINUX_INVALID_STUB(sysfs)
LINUX_PRIVILEGED_STUB(vhangup)
LINUX_PRIVILEGED_STUB(modify_ldt)
LINUX_PRIVILEGED_STUB(pivot_root)
LINUX_INVALID_STUB(_sysctl)
LINUX_PRIVILEGED_STUB(adjtimex)
LINUX_PRIVILEGED_STUB(acct)
LINUX_PRIVILEGED_STUB(settimeofday)
LINUX_PRIVILEGED_STUB(swapon)
LINUX_PRIVILEGED_STUB(swapoff)
LINUX_PRIVILEGED_STUB(iopl)
LINUX_PRIVILEGED_STUB(ioperm)
LINUX_PRIVILEGED_STUB(create_module)
LINUX_PRIVILEGED_STUB(init_module)
LINUX_PRIVILEGED_STUB(delete_module)
LINUX_INVALID_STUB(get_kernel_syms)
LINUX_INVALID_STUB(query_module)
LINUX_NODEV_STUB(quotactl)
LINUX_INVALID_STUB(nfsservctl)
LINUX_INVALID_STUB(getpmsg)
LINUX_INVALID_STUB(putpmsg)
LINUX_INVALID_STUB(afs_syscall)
LINUX_INVALID_STUB(tuxcall)
LINUX_INVALID_STUB(security)
LINUX_INVALID_STUB(io_setup)
LINUX_INVALID_STUB(io_destroy)
LINUX_INVALID_STUB(io_getevents)
LINUX_INVALID_STUB(io_submit)
LINUX_INVALID_STUB(io_cancel)
LINUX_INVALID_STUB(lookup_dcookie)
LINUX_INVALID_STUB(epoll_ctl_old)
LINUX_INVALID_STUB(epoll_wait_old)
LINUX_INVALID_STUB(remap_file_pages)
LINUX_INVALID_STUB(timer_create)
LINUX_INVALID_STUB(timer_settime)
LINUX_INVALID_STUB(timer_gettime)
LINUX_INVALID_STUB(timer_getoverrun)
LINUX_INVALID_STUB(timer_delete)
LINUX_INVALID_STUB(vserver)
LINUX_INVALID_STUB(mbind)
LINUX_INVALID_STUB(set_mempolicy)
LINUX_INVALID_STUB(get_mempolicy)
LINUX_INVALID_STUB(mq_open)
LINUX_INVALID_STUB(mq_unlink)
LINUX_INVALID_STUB(mq_timedsend)
LINUX_INVALID_STUB(mq_timedreceive)
LINUX_INVALID_STUB(mq_notify)
LINUX_INVALID_STUB(mq_getsetattr)
LINUX_PRIVILEGED_STUB(kexec_load)
LINUX_INVALID_STUB(add_key)
LINUX_INVALID_STUB(request_key)
LINUX_INVALID_STUB(keyctl)
LINUX_INVALID_STUB(migrate_pages)
LINUX_PRIVILEGED_STUB(unshare)
LINUX_INVALID_STUB(splice)
LINUX_INVALID_STUB(tee)
LINUX_INVALID_STUB(vmsplice)
LINUX_INVALID_STUB(move_pages)
LINUX_INVALID_STUB(rt_tgsigqueueinfo)
LINUX_PRIVILEGED_STUB(perf_event_open)
LINUX_INVALID_STUB(fanotify_init)
LINUX_INVALID_STUB(fanotify_mark)
LINUX_INVALID_STUB(name_to_handle_at)
LINUX_PRIVILEGED_STUB(open_by_handle_at)
LINUX_PRIVILEGED_STUB(clock_adjtime)
LINUX_PRIVILEGED_STUB(setns)
LINUX_INVALID_STUB(process_vm_readv)
LINUX_INVALID_STUB(process_vm_writev)
LINUX_INVALID_STUB(kcmp)
LINUX_PRIVILEGED_STUB(finit_module)
LINUX_INVALID_STUB(seccomp)
LINUX_PRIVILEGED_STUB(kexec_file_load)
LINUX_PRIVILEGED_STUB(bpf)
LINUX_INVALID_STUB(execveat)
LINUX_INVALID_STUB(userfaultfd)
LINUX_INVALID_STUB(pkey_mprotect)
LINUX_INVALID_STUB(pkey_alloc)
LINUX_INVALID_STUB(pkey_free)
LINUX_INVALID_STUB(io_pgetevents)
LINUX_INVALID_STUB(pidfd_send_signal)
LINUX_INVALID_STUB(io_uring_setup)
LINUX_INVALID_STUB(io_uring_enter)
LINUX_INVALID_STUB(io_uring_register)
LINUX_PRIVILEGED_STUB(open_tree)
LINUX_PRIVILEGED_STUB(move_mount)
LINUX_INVALID_STUB(fsopen)
LINUX_INVALID_STUB(fsconfig)
LINUX_INVALID_STUB(fsmount)
LINUX_INVALID_STUB(fspick)
LINUX_INVALID_STUB(pidfd_open)
LINUX_INVALID_STUB(pidfd_getfd)
LINUX_INVALID_STUB(process_madvise)
LINUX_PRIVILEGED_STUB(mount_setattr)
LINUX_NODEV_STUB(quotactl_fd)
LINUX_INVALID_STUB(landlock_create_ruleset)
LINUX_INVALID_STUB(landlock_add_rule)
LINUX_INVALID_STUB(landlock_restrict_self)
LINUX_INVALID_STUB(memfd_secret)
LINUX_INVALID_STUB(process_mrelease)
LINUX_INVALID_STUB(futex_waitv)
LINUX_INVALID_STUB(set_mempolicy_home_node)
LINUX_INVALID_STUB(cachestat)
LINUX_INVALID_STUB(map_shadow_stack)
LINUX_INVALID_STUB(futex_wake)
LINUX_INVALID_STUB(futex_wait)
LINUX_INVALID_STUB(futex_requeue)
LINUX_INVALID_STUB(statmount)
LINUX_INVALID_STUB(listmount)
LINUX_INVALID_STUB(lsm_get_self_attr)
LINUX_INVALID_STUB(lsm_set_self_attr)
LINUX_INVALID_STUB(lsm_list_modules)
