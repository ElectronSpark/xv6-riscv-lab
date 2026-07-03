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
#include "proc_private.h"
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
#include "proc/cred.h"
#include "vfs/file.h"
#include "smp/percpu.h"
#include "proc/chrome_lifecycle.h"
#include "klog.h"
#include <mm/slab.h>

/* Forward declarations for arg helpers */
void argint(int n, int *ip);
void argint64(int n, int64 *ip);
void argaddr(int n, uint64 *ip);
int argstr(int n, char *buf, int max);

/* ================================================================== */
/*  prctl                                                             */
/* ================================================================== */

#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_GET_SECCOMP 21
#define PR_SET_SECCOMP 22
#define PR_CAPBSET_READ 23
#define PR_CAPBSET_DROP 24
#define PR_GET_SECUREBITS 27
#define PR_SET_SECUREBITS 28
#define PR_SET_TIMERSLACK 29
#define PR_GET_TIMERSLACK 30
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0

#define CAP_LAST_CAP 40
#define DEFAULT_TIMER_SLACK_NS 50000ULL

uint64 sys_prctl(void) {
    int option;
    uint64 arg2, arg3, arg4, arg5;
    argint(0, &option);
    argaddr(1, &arg2);
    argaddr(2, &arg3);
    argaddr(3, &arg4);
    argaddr(4, &arg5);

    switch (option) {
    case PR_GET_DUMPABLE: {
        struct thread_group *tg = current->thread_group;
        if (arg2 || arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        return tg != NULL ? __atomic_load_n(&tg->dumpable, __ATOMIC_SEQ_CST) : 1;
    }
    case PR_SET_DUMPABLE: {
        struct thread_group *tg = current->thread_group;
        if (arg2 > 2 || arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        if (tg != NULL)
            __atomic_store_n(&tg->dumpable, (int)arg2, __ATOMIC_SEQ_CST);
        return 0;
    }
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
    case PR_GET_SECCOMP:
        if (arg2 || arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        return 0;
    case PR_SET_SECCOMP:
        /*
         * The current kernel has no seccomp filter engine.  Match Linux's
         * disabled-sandbox shape: strict/filter requests are unsupported,
         * but the option itself is recognized.
         */
        return (uint64)-EINVAL;
    case PR_CAPBSET_READ:
        if (arg2 > CAP_LAST_CAP)
            return (uint64)-EINVAL;
        return 1;
    case PR_CAPBSET_DROP:
        if (arg2 > CAP_LAST_CAP || arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        return 0;
    case PR_GET_SECUREBITS:
        if (arg2 || arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        return 0;
    case PR_SET_SECUREBITS:
        if (arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        return arg2 == 0 ? 0 : (uint64)-EINVAL;
    case PR_SET_TIMERSLACK: {
        struct thread_group *tg = current->thread_group;
        if (arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        if (tg != NULL) {
            uint64 slack = arg2 != 0 ? arg2 : DEFAULT_TIMER_SLACK_NS;
            __atomic_store_n(&tg->timer_slack_ns, slack, __ATOMIC_SEQ_CST);
        }
        return 0;
    }
    case PR_GET_TIMERSLACK: {
        struct thread_group *tg = current->thread_group;
        if (arg2 || arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        return tg != NULL ?
            __atomic_load_n(&tg->timer_slack_ns, __ATOMIC_SEQ_CST) :
            DEFAULT_TIMER_SLACK_NS;
    }
    case PR_SET_NO_NEW_PRIVS: {
        struct thread_group *tg = current->thread_group;
        if (arg2 != 1 || arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        if (tg != NULL)
            __atomic_store_n(&tg->no_new_privs, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    case PR_GET_NO_NEW_PRIVS: {
        struct thread_group *tg = current->thread_group;
        if (arg2 || arg3 || arg4 || arg5)
            return (uint64)-EINVAL;
        return tg != NULL ?
            __atomic_load_n(&tg->no_new_privs, __ATOMIC_SEQ_CST) : 0;
    }
    case PR_SET_VMA:
        if (arg2 != PR_SET_VMA_ANON_NAME || arg4 == 0)
            return (uint64)-EINVAL;
        return 0;
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
        int priority = target->sched_entity->priority;
        if (IS_EEVDF_PRIORITY(priority))
            nice = priority - EEVDF_PRIORITY_START - LINUX_NICE_BIAS;
    }

    return (uint64)(LINUX_NICE_BIAS - nice);
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
        attr.priority = EEVDF_PRIORITY_START + niceval + LINUX_NICE_BIAS;
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

#define RSEQ_CPU_ID_UNINITIALIZED      (-1)
#define RSEQ_CPU_ID_REGISTRATION_FAILED (-2)
#define RSEQ_FLAG_UNREGISTER           (1U << 0)
#define RSEQ_ABI_SIZE                  32U
#define RSEQ_ABI_ALIGN                 32U
#define RSEQ_SIG_SIZE                  4U
#define RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT (1U << 0)
#define RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL  (1U << 1)
#define RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE (1U << 2)

struct linux_rseq_abi {
    uint32 cpu_id_start;
    uint32 cpu_id;
    uint64 rseq_cs;
    uint32 flags;
    uint32 node_id;
    uint32 mm_cid;
    uint32 padding;
} __attribute__((packed, aligned(4)));
_Static_assert(sizeof(struct linux_rseq_abi) == RSEQ_ABI_SIZE,
               "Linux rseq ABI struct must be 32 bytes");

struct linux_rseq_cs_abi {
    uint32 version;
    uint32 flags;
    uint64 start_ip;
    uint64 post_commit_offset;
    uint64 abort_ip;
} __attribute__((packed));
_Static_assert(sizeof(struct linux_rseq_cs_abi) == 32,
               "Linux rseq_cs ABI struct must be 32 bytes");

static int rseq_copyout_cpu(struct thread *p, int cpu)
{
    struct linux_rseq_abi rseq;

    if (p == NULL || p->vm == NULL || p->rseq_addr == 0)
        return 0;

    if (vm_copyin(p->vm, &rseq, p->rseq_addr, sizeof(rseq)) < 0)
        return -EFAULT;

    rseq.cpu_id_start = (uint32)cpu;
    rseq.cpu_id = (uint32)cpu;
    rseq.node_id = 0;
    rseq.mm_cid = (uint32)p->pid;

    if (vm_copyout(p->vm, p->rseq_addr, &rseq, sizeof(rseq)) < 0)
        return -EFAULT;
    p->rseq_cpu_id = cpu;
    return 0;
}

static int rseq_mark_unregistered(struct thread *p)
{
    uint32 cpu_id = (uint32)RSEQ_CPU_ID_UNINITIALIZED;

    if (p == NULL || p->vm == NULL || p->rseq_addr == 0)
        return 0;
    if (vm_copyout(p->vm, p->rseq_addr + 4, &cpu_id, sizeof(cpu_id)) < 0)
        return -EFAULT;
    return 0;
}

static int rseq_event_restart_allowed(uint32 cs_flags, uint32 events)
{
    if ((events & RSEQ_EVENT_PREEMPT) &&
        !(cs_flags & RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT))
        return 1;
    if ((events & RSEQ_EVENT_SIGNAL) &&
        !(cs_flags & RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL))
        return 1;
    if ((events & RSEQ_EVENT_MIGRATE) &&
        !(cs_flags & RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE))
        return 1;
    return 0;
}

static int rseq_abort_active_cs(struct thread *p, uint32 events)
{
    struct linux_rseq_abi rseq;
    struct linux_rseq_cs_abi cs;
    uint64 ip;
    uint32 abort_sig = 0;
    uint64 cs_end;
    uint64 zero = 0;

    if (p == NULL || p->vm == NULL || p->trapframe == NULL ||
        p->rseq_addr == 0 || events == 0)
        return 0;

    if (vm_copyin(p->vm, &rseq, p->rseq_addr, sizeof(rseq)) < 0)
        return -EFAULT;
    if (rseq.rseq_cs == 0)
        return 0;
    if (vm_copyin(p->vm, &cs, rseq.rseq_cs, sizeof(cs)) < 0)
        return -EFAULT;
    if (cs.version != 0)
        return -EINVAL;
    if (!rseq_event_restart_allowed(cs.flags, events))
        return 0;
    if (cs.post_commit_offset == 0 ||
        cs.post_commit_offset > (~0ULL - cs.start_ip))
        return -EINVAL;

    ip = p->trapframe->trapframe.sepc;
    cs_end = cs.start_ip + cs.post_commit_offset;
    if (ip < cs.start_ip || ip >= cs_end)
        return 0;

    if (cs.abort_ip >= RSEQ_SIG_SIZE) {
        if (vm_copyin(p->vm, &abort_sig, cs.abort_ip - RSEQ_SIG_SIZE,
                      sizeof(abort_sig)) < 0)
            return -EFAULT;
        if (abort_sig != p->rseq_signature)
            return -EINVAL;
    } else {
        return -EINVAL;
    }

    if (vm_copyout(p->vm, p->rseq_addr + 8, &zero, sizeof(zero)) < 0)
        return -EFAULT;
    p->trapframe->trapframe.sepc = cs.abort_ip;
    return 0;
}

void rseq_user_return(struct thread *p, uint32 events)
{
    int cpu;
    uint32 effective_events = events;

    if (p == NULL || p->rseq_addr == 0 || p->vm == NULL)
        return;

    cpu = cpuid();
    if (p->rseq_cpu_id >= 0 && p->rseq_cpu_id != cpu)
        effective_events |= RSEQ_EVENT_MIGRATE;

    if (effective_events == 0 && p->rseq_cpu_id == cpu)
        return;

    if (rseq_abort_active_cs(p, effective_events) < 0 ||
        rseq_copyout_cpu(p, cpu) < 0) {
        /*
         * Linux treats a bad registered rseq area as a task fault.  Mark the
         * thread for SIGSEGV so usertrapret's existing signal/exit path owns
         * the final disposition instead of silently publishing stale state.
         */
        kill_thread(p, SIGSEGV);
    }
}

void rseq_clear_thread(struct thread *p)
{
    if (p == NULL)
        return;
    p->rseq_addr = 0;
    p->rseq_len = 0;
    p->rseq_signature = 0;
    p->rseq_cpu_id = RSEQ_CPU_ID_UNINITIALIZED;
}

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

uint64 sys_rseq(void) {
    uint64 rseq_addr;
    uint64 rseq_len64;
    int flags;
    uint64 signature64;
    uint32 signature;

    argaddr(0, &rseq_addr);
    argaddr(1, &rseq_len64);
    argint(2, &flags);
    argaddr(3, &signature64);
    signature = (uint32)signature64;

    if ((uint32)flags & ~RSEQ_FLAG_UNREGISTER)
        return (uint64)-EINVAL;

    if ((uint32)flags & RSEQ_FLAG_UNREGISTER) {
        if ((uint32)flags != RSEQ_FLAG_UNREGISTER)
            return (uint64)-EINVAL;
        if (current->rseq_addr == 0)
            return (uint64)-EINVAL;
        if (rseq_addr != current->rseq_addr ||
            (uint32)rseq_len64 != current->rseq_len ||
            signature != current->rseq_signature)
            return (uint64)-EINVAL;
        if (rseq_mark_unregistered(current) < 0)
            return (uint64)-EFAULT;
        rseq_clear_thread(current);
        return 0;
    }

    if (rseq_addr == 0 || (rseq_addr & (RSEQ_ABI_ALIGN - 1)) != 0)
        return (uint64)-EINVAL;
    if (rseq_len64 < RSEQ_ABI_SIZE || rseq_len64 > UINT32_MAX)
        return (uint64)-EINVAL;
    if (current->rseq_addr != 0)
        return (uint64)-EBUSY;

    current->rseq_addr = rseq_addr;
    current->rseq_len = (uint32)rseq_len64;
    current->rseq_signature = signature;
    current->rseq_cpu_id = RSEQ_CPU_ID_UNINITIALIZED;

    if (rseq_copyout_cpu(current, cpuid()) < 0) {
        rseq_clear_thread(current);
        return (uint64)-EFAULT;
    }

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
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5
#define SCHED_DEADLINE 6
#define SCHED_RESET_ON_FORK 0x40000000
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

static int linux_sched_policy_base(int policy) {
    return policy & ~SCHED_RESET_ON_FORK;
}

static int linux_sched_priority_valid(int policy, int priority) {
    switch (linux_sched_policy_base(policy)) {
    case SCHED_FIFO:
    case SCHED_RR:
        return priority >= 1 && priority <= 99;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
        return priority == 0;
    default:
        return 0;
    }
}

static int kde_sched_trace_enabled(void)
{
    static int initialized;
    static int enabled;

    if (!initialized) {
        enabled = chrome_trace_value_enabled("kde_sched_trace");
        initialized = 1;
    }
    return enabled;
}

static int kde_sched_trace_current(void)
{
    if (current == NULL)
        return 0;
    if (strncmp(current->name, "wireplumber", 11) == 0 ||
        strncmp(current->name, "pipewire", 8) == 0 ||
        strncmp(current->name, "pipewire-pulse", 14) == 0)
        return 1;
    if (current->thread_group == NULL ||
        current->thread_group->exec_path[0] == '\0')
        return 0;
    return strstr(current->thread_group->exec_path, "wireplumber") != NULL ||
           strstr(current->thread_group->exec_path, "pipewire") != NULL;
}

static void kde_sched_trace(const char *op, int pid, int policy, int priority,
                            int ret)
{
    if (!kde_sched_trace_enabled() || !kde_sched_trace_current())
        return;

    printf("kde-sched-trace: op=%s pid=%d tgid=%d name=%s arg_pid=%d "
           "policy=%d base=%d priority=%d ret=%d stored_policy=%d "
           "stored_priority=%d\n",
           op, current->pid, current->tgid, current->name, pid, policy,
           linux_sched_policy_base(policy), priority, ret,
           current->linux_sched_policy, current->linux_sched_priority);
}

uint64 sys_sched_getparam(void) {
    int pid;
    uint64 param_addr;
    argint(0, &pid);
    argaddr(1, &param_addr);
    if (param_addr == 0)
        return (uint64)-EFAULT;
    if (pid < 0)
        return (uint64)-EINVAL;

    struct thread *target = current;
    if (pid != 0 && get_pid_thread(pid, &target) != 0)
        return (uint64)-ESRCH;

    int priority = target != NULL ? target->linux_sched_priority : 0;
    struct linux_sched_param param = { .sched_priority = priority };
    if (either_copyout(1, param_addr, &param, sizeof(param)) < 0) {
        kde_sched_trace("getparam", pid, target != NULL ?
                        target->linux_sched_policy : SCHED_OTHER, priority,
                        -EFAULT);
        return (uint64)-EFAULT;
    }
    kde_sched_trace("getparam", pid, target != NULL ?
                    target->linux_sched_policy : SCHED_OTHER, priority, 0);
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
    struct thread *target = current;
    if (pid != 0 && get_pid_thread(pid, &target) != 0)
        return (uint64)-ESRCH;

    int policy = target != NULL ? target->linux_sched_policy : SCHED_OTHER;
    if (linux_sched_policy_base(policy) == SCHED_FIFO ||
        linux_sched_policy_base(policy) == SCHED_RR) {
        kde_sched_trace("setparam", pid, policy, param.sched_priority,
                        -EPERM);
        return (uint64)-EPERM;
    }
    if (!linux_sched_priority_valid(policy, param.sched_priority)) {
        kde_sched_trace("setparam", pid, policy, param.sched_priority,
                        -EINVAL);
        return (uint64)-EINVAL;
    }
    if (target != NULL)
        target->linux_sched_priority = param.sched_priority;
    kde_sched_trace("setparam", pid, policy, param.sched_priority, 0);
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
    struct thread *target = current;
    if (pid != 0 && get_pid_thread(pid, &target) != 0)
        return (uint64)-ESRCH;

    int nice = 0;
    if (target != NULL && target->sched_entity != NULL) {
        int priority = target->sched_entity->priority;
        if (IS_EEVDF_PRIORITY(priority))
            nice = priority - EEVDF_PRIORITY_START - LINUX_NICE_BIAS;
    }

    attr.sched_nice = nice;
    if (target != NULL) {
        attr.sched_policy = target->linux_sched_policy;
        attr.sched_priority = target->linux_sched_priority;
    }

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

    if ((attr.sched_policy & SCHED_RESET_ON_FORK) != 0 ||
        attr.sched_flags != 0 || attr.sched_runtime != 0 ||
        attr.sched_deadline != 0 || attr.sched_period != 0)
        return (uint64)-EINVAL;
    if (linux_sched_policy_base(attr.sched_policy) == SCHED_FIFO ||
        linux_sched_policy_base(attr.sched_policy) == SCHED_RR)
        return (uint64)-EPERM;
    if (!linux_sched_priority_valid(attr.sched_policy, attr.sched_priority))
        return (uint64)-EINVAL;

    if (attr.sched_nice < -20 || attr.sched_nice > 19)
        return (uint64)-EINVAL;
    if (attr.sched_nice < 0 && current->thread_group->euid != 0)
        return (uint64)-EACCES;

    struct thread *target = current;
    if (pid != 0 && get_pid_thread(pid, &target) != 0)
        return (uint64)-ESRCH;

    if (target != NULL) {
        target->linux_sched_policy = linux_sched_policy_base(attr.sched_policy);
        target->linux_sched_priority = attr.sched_priority;
    }
    if (target != NULL && target->sched_entity != NULL &&
        linux_sched_policy_base(attr.sched_policy) == SCHED_OTHER) {
        struct sched_attr sattr;
        sched_attr_init(&sattr);
        sattr.priority =
            EEVDF_PRIORITY_START + attr.sched_nice + LINUX_NICE_BIAS;
        int ret = sched_setattr(target->sched_entity, &sattr);
        if (ret < 0)
            return (uint64)ret;
    }
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

    /* Keep this consistent with /sys/devices/system/cpu/{online,present,possible}. */
    uint8 buf[128];
    int len = cpusetsize;
    cpumask_t mask = cpu_possible_mask();
    if (len > (int)sizeof(buf))
        len = (int)sizeof(buf);
    memset(buf, 0, len);
    memcpy(buf, &mask, len < (int)sizeof(mask) ? len : (int)sizeof(mask));

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
    int pid;
    argint(0, &pid);
    if (pid < 0)
        return (uint64)-EINVAL;

    struct thread *target = current;
    if (pid != 0 && get_pid_thread(pid, &target) != 0)
        return (uint64)-ESRCH;
    int policy = target != NULL ? target->linux_sched_policy : SCHED_OTHER;
    kde_sched_trace("getscheduler", pid, policy,
                    target != NULL ? target->linux_sched_priority : 0,
                    policy);
    return (uint64)policy;
}

uint64 sys_sched_setscheduler(void) {
    int pid;
    int policy;
    uint64 param_addr;
    argint(0, &pid);
    argint(1, &policy);
    argaddr(2, &param_addr);

    if (pid < 0)
        return (uint64)-EINVAL;
    if (param_addr == 0)
        return (uint64)-EFAULT;

    int base_policy = linux_sched_policy_base(policy);
    if (base_policy == SCHED_DEADLINE) {
        kde_sched_trace("setscheduler", pid, policy, 0, -EINVAL);
        return (uint64)-EINVAL;
    }
    if (base_policy == SCHED_FIFO || base_policy == SCHED_RR) {
        kde_sched_trace("setscheduler", pid, policy, 0, -EPERM);
        return (uint64)-EPERM;
    }

    struct linux_sched_param param;
    if (either_copyin(&param, 1, param_addr, sizeof(param)) < 0)
        return (uint64)-EFAULT;
    if (!linux_sched_priority_valid(policy, param.sched_priority)) {
        kde_sched_trace("setscheduler", pid, policy, param.sched_priority,
                        -EINVAL);
        return (uint64)-EINVAL;
    }

    struct thread *target = current;
    if (pid != 0 && get_pid_thread(pid, &target) != 0)
        return (uint64)-ESRCH;
    if (target != NULL) {
        target->linux_sched_policy = base_policy;
        target->linux_sched_priority = param.sched_priority;
    }
    kde_sched_trace("setscheduler", pid, policy, param.sched_priority, 0);
    return 0;
}

/* ================================================================== */
/*  sched_get_priority_max / sched_get_priority_min                   */
/* ================================================================== */

uint64 sys_sched_get_priority_max(void) {
    int policy;

    argint(0, &policy);
    policy = linux_sched_policy_base(policy);
    switch (policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        kde_sched_trace("get_priority_max", -1, policy, 99, 99);
        return 99;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
    case SCHED_DEADLINE:
        kde_sched_trace("get_priority_max", -1, policy, 0, 0);
        return 0;
    default:
        kde_sched_trace("get_priority_max", -1, policy, 0, -EINVAL);
        return (uint64)-EINVAL;
    }
}

uint64 sys_sched_get_priority_min(void) {
    int policy;

    argint(0, &policy);
    policy = linux_sched_policy_base(policy);
    switch (policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        kde_sched_trace("get_priority_min", -1, policy, 1, 1);
        return 1;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
    case SCHED_DEADLINE:
        kde_sched_trace("get_priority_min", -1, policy, 0, 0);
        return 0;
    default:
        kde_sched_trace("get_priority_min", -1, policy, 0, -EINVAL);
        return (uint64)-EINVAL;
    }
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
    if (size < 64 || size > 4096)
        return (uint64)-EINVAL;

    /* Copy in the Linux clone_args (up to what we support) */
    struct linux_clone_args largs;
    memset(&largs, 0, sizeof(largs));

    uint64 copy_size = size;
    if (copy_size > sizeof(largs))
        copy_size = sizeof(largs);

    if (vm_copyin(current->vm, &largs, uargs_addr, copy_size) < 0)
        return (uint64)-EFAULT;
    if (size > sizeof(largs)) {
        uint8 extra[64];
        uint64 pos = sizeof(largs);

        while (pos < size) {
            uint64 chunk = size - pos;
            if (chunk > sizeof(extra))
                chunk = sizeof(extra);
            if (vm_copyin(current->vm, extra, uargs_addr + pos, chunk) < 0)
                return (uint64)-EFAULT;
            for (uint64 i = 0; i < chunk; i++) {
                if (extra[i] != 0)
                    return (uint64)-E2BIG;
            }
            pos += chunk;
        }
    }

    /*
     * Support the Linux clone3 subset needed by glibc/Chromium thread and
     * process creation.  Features that need extra Linux objects must fail
     * explicitly rather than being silently ignored.
     */
    uint64 unsupported_linux_clone3_flags =
        CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS |
        CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET |
        CLONE_INTO_CGROUP | CLONE_PID | CLONE_SYSTEM | CLONE_SIGSTOPPED;
    if ((largs.flags & unsupported_linux_clone3_flags) != 0 ||
        largs.cgroup != 0 || largs.set_tid != 0 || largs.set_tid_size != 0) {
        return (uint64)-EINVAL;
    }
    if ((largs.flags & (CLONE_CLEAR_SIGHAND | CLONE_SIGHAND)) ==
        (CLONE_CLEAR_SIGHAND | CLONE_SIGHAND)) {
        return (uint64)-EINVAL;
    }
    if ((largs.flags & CLONE_PIDFD) != 0) {
        if ((largs.flags & CLONE_DETACHED) != 0)
            return (uint64)-EINVAL;
        if (largs.pidfd == 0)
            return (uint64)-EFAULT;
    }
    if (largs.exit_signal != 0 && SIGBAD(largs.exit_signal))
        return (uint64)-EINVAL;

    uint64 linux_clone_mask =
        0xffULL | CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
        CLONE_PIDFD | CLONE_PTRACE | CLONE_VFORK | CLONE_PARENT |
        CLONE_THREAD | CLONE_NEWNS | CLONE_SYSVSEM | CLONE_SETTLS |
        CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED |
        CLONE_UNTRACED | CLONE_CHILD_SETTID | CLONE_NEWCGROUP |
        CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
        CLONE_NEWNET | CLONE_IO | CLONE_CLEAR_SIGHAND |
        CLONE_INTO_CGROUP | CLONE_PID | CLONE_SYSTEM | CLONE_SIGSTOPPED;
    if ((largs.flags & ~linux_clone_mask) != 0)
        return (uint64)-EINVAL;

    /* Map to xv6 clone_args */
    struct clone_args args = {
        .flags      = largs.flags & ~0xffULL,
        .stack      = largs.stack,
        .stack_size = largs.stack_size,
        .entry      = 0,
        .esignal    = largs.exit_signal ? largs.exit_signal : (largs.flags & 0xff),
        .tls        = largs.tls,
        .ctid       = largs.child_tid,
        .ptid       = largs.parent_tid,
        .pidfd      = largs.pidfd,
    };

    return (uint64)thread_clone(&args);
}

/* ================================================================== */
/*  Memory protection keys                                             */
/* ================================================================== */

#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE  0x2
#define PKEY_ACCESS_MASK    (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE)

/*
 * xv6 does not currently expose x86 PKU state to user space.  Keep the Linux
 * ABI shape: applications can probe pkey_alloc() and learn that no key is
 * available, while pkey_mprotect(..., -1) is normal mprotect().  Accept pkey 0
 * too because Linux reserves it as the default protection domain.
 */
uint64 sys_pkey_alloc(void)
{
    uint64 flags, access_rights;
    argaddr(0, &flags);
    argaddr(1, &access_rights);

    if (flags != 0 || (access_rights & ~PKEY_ACCESS_MASK) != 0)
        return (uint64)-EINVAL;
    return (uint64)-ENOSPC;
}

uint64 sys_pkey_free(void)
{
    return (uint64)-EINVAL;
}

uint64 sys_pkey_mprotect(void)
{
    uint64 addr, length;
    int prot, pkey;

    argaddr(0, &addr);
    argaddr(1, &length);
    argint(2, &prot);
    argint(3, &pkey);

    if (pkey != -1 && pkey != 0)
        return (uint64)-EINVAL;
    if (length == 0)
        return (uint64)-EINVAL;
    return (uint64)vm_mprotect(current->vm, addr, (size_t)length, prot);
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
/*  kcmp                                                              */
/* ================================================================== */

#define KCMP_FILE 0

uint64 sys_kcmp(void)
{
    int pid1, pid2, type;
    uint64 idx1, idx2;
    struct vfs_file *file1;
    struct vfs_file *file2;
    uint64 self_pid;
    int ret;

    argint(0, &pid1);
    argint(1, &pid2);
    argint(2, &type);
    argaddr(3, &idx1);
    argaddr(4, &idx2);

    if (type != KCMP_FILE)
        return (uint64)-EINVAL;
    if (idx1 >= NOFILE || idx2 >= NOFILE)
        return (uint64)-EBADF;
    if (current == NULL || current->fdtable == NULL)
        return (uint64)-ESRCH;

    self_pid = (uint64)thread_tgid(current);
    if (pid1 != (int)self_pid || pid2 != (int)self_pid)
        return (uint64)-EPERM;

    file1 = vfs_fdtable_get_file(current->fdtable, (int)idx1);
    if (file1 == NULL)
        return (uint64)-EBADF;
    file2 = vfs_fdtable_get_file(current->fdtable, (int)idx2);
    if (file2 == NULL) {
        vfs_fput(file1);
        return (uint64)-EBADF;
    }

    if (file1 == file2)
        ret = 0;
    else
        ret = (file1 < file2) ? 1 : 2;

    vfs_fput(file2);
    vfs_fput(file1);
    return (uint64)ret;
}

/* ================================================================== */
/*  Linux optional/privileged compatibility syscalls                   */
/* ================================================================== */

#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#define LANDLOCK_CREATE_RULESET_ERRATA  (1U << 1)
#define LANDLOCK_ABI_VERSION 7
#define LANDLOCK_FIXED_ERRATA 0

struct landlock_ruleset_attr_compat {
    uint64 handled_access_fs;
    uint64 handled_access_net;
};

uint64 sys_landlock_create_ruleset(void)
{
    uint64 attr_addr;
    uint64 size;
    int flags;
    argaddr(0, &attr_addr);
    argaddr(1, &size);
    argint(2, &flags);

    const int query_flags =
        LANDLOCK_CREATE_RULESET_VERSION | LANDLOCK_CREATE_RULESET_ERRATA;
    if ((flags & ~query_flags) != 0)
        return (uint64)-EINVAL;

    if (flags != 0) {
        if (attr_addr != 0 || size != 0)
            return (uint64)-EINVAL;
        if (flags == LANDLOCK_CREATE_RULESET_VERSION)
            return LANDLOCK_ABI_VERSION;
        if (flags == LANDLOCK_CREATE_RULESET_ERRATA)
            return LANDLOCK_FIXED_ERRATA;
        return (uint64)-EINVAL;
    }

    if (attr_addr == 0)
        return (uint64)-EFAULT;
    if (size < sizeof(uint64))
        return (uint64)-EINVAL;
    if (size > sizeof(struct landlock_ruleset_attr_compat))
        return (uint64)-E2BIG;

    struct landlock_ruleset_attr_compat attr;
    memset(&attr, 0, sizeof(attr));
    if (vm_copyin(current->vm, &attr, attr_addr, size) < 0)
        return (uint64)-EFAULT;
    if (attr.handled_access_fs == 0 && attr.handled_access_net == 0)
        return (uint64)-ENOMSG;

    /*
     * The Chromium-observed query path needs Linux's ABI-version result, but
     * xv6 does not yet have a Landlock enforcement engine.  Do not create an
     * inert ruleset fd or allow a no-op restrict_self(); report the same
     * fail-closed shape Linux uses when Landlock is supported but disabled.
     */
    return (uint64)-EOPNOTSUPP;
}

uint64 sys_landlock_add_rule(void)
{
    int flags;
    argint(3, &flags);
    if (flags != 0)
        return (uint64)-EINVAL;
    return (uint64)-EOPNOTSUPP;
}

uint64 sys_landlock_restrict_self(void)
{
    int flags;
    argint(1, &flags);
    if (flags != 0)
        return (uint64)-EINVAL;
    return (uint64)-EOPNOTSUPP;
}

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

/* ================================================================== */
/*  ptrace                                                            */
/* ================================================================== */

#define PTRACE_TRACEME      0
#define PTRACE_PEEKTEXT     1
#define PTRACE_PEEKDATA     2
#define PTRACE_PEEKUSR      3
#define PTRACE_POKETEXT     4
#define PTRACE_POKEDATA     5
#define PTRACE_CONT         7
#define PTRACE_ATTACH       16
#define PTRACE_DETACH       17
#define PTRACE_SETOPTIONS   0x4200
#define PTRACE_GETEVENTMSG  0x4201
#define PTRACE_GETSIGINFO   0x4202
#define PTRACE_GETREGSET    0x4204
#define PTRACE_SEIZE        0x4206
#define PTRACE_INTERRUPT    0x4207

#define PTRACE_O_MASK       0x003000ffUL
#define NT_PRSTATUS         1
#define NT_FPREGSET         2

struct ptrace_iovec {
    uint64 iov_base;
    uint64 iov_len;
};

struct ptrace_x86_user_regs {
    uint64 r15;
    uint64 r14;
    uint64 r13;
    uint64 r12;
    uint64 rbp;
    uint64 rbx;
    uint64 r11;
    uint64 r10;
    uint64 r9;
    uint64 r8;
    uint64 rax;
    uint64 rcx;
    uint64 rdx;
    uint64 rsi;
    uint64 rdi;
    uint64 orig_rax;
    uint64 rip;
    uint64 cs;
    uint64 eflags;
    uint64 rsp;
    uint64 ss;
    uint64 fs_base;
    uint64 gs_base;
    uint64 ds;
    uint64 es;
    uint64 fs;
    uint64 gs;
};

static int ptrace_trace_caller(void)
{
    if (current == NULL)
        return 0;
    if (!chrome_trace_value_enabled("chrome_ptrace_trace") &&
        !chrome_trace_value_enabled("chrome_syscall_trace"))
        return 0;
    return strstr(current->name, "chrome") != NULL ||
           strstr(current->name, "crashpad") != NULL ||
           chrome_lifecycle_thread_match(current);
}

static void ptrace_trace_result(int request, int pid, uint64 addr, uint64 data,
                                int ret)
{
    struct thread_group *ctg = current != NULL ? current->thread_group : NULL;
    struct thread *target = NULL;
    struct thread_group *ttg = NULL;
    int target_state = -1;
    int target_pid = 0;
    int target_tgid = 0;
    int target_tracer_tgid = 0;
    int target_dumpable = -1;
    int target_uid = -1;
    int target_euid = -1;
    int current_uid = ctg != NULL ? ctg->uid : -1;
    int current_euid = ctg != NULL ? ctg->euid : -1;
    int current_tgid = current != NULL ? thread_tgid(current) : -1;
    const char *target_name = "?";
    int target_is_chrome = 0;

    if (!ptrace_trace_caller())
        return;

    pid_rlock();
    if (pid > 0 && get_pid_thread(pid, &target) == 0 && target != NULL) {
        ttg = target->thread_group;
        target_pid = target->pid;
        target_tgid = target->tgid;
        target_name = target->name;
        target_state = __thread_state_get(target);
        target_tracer_tgid = target->ptrace_tracer_tgid;
        if (ttg != NULL) {
            target_dumpable =
                __atomic_load_n(&ttg->dumpable, __ATOMIC_ACQUIRE);
            target_uid = ttg->uid;
            target_euid = ttg->euid;
        }
        target_is_chrome = chrome_lifecycle_thread_match(target);
    }

    printf("ptrace: request=%d pid=%d addr=0x%lx data=0x%lx ret=%d "
           "caller='%s' caller_pid=%d caller_tgid=%d uid=%d euid=%d "
           "target='%s' target_pid=%d target_tgid=%d state=%d "
           "tracer_tgid=%d dumpable=%d target_uid=%d target_euid=%d "
           "target_chrome=%d\n",
           request, pid, addr, data, ret, current ? current->name : "?",
           current ? current->pid : -1, current_tgid, current_uid,
           current_euid, target_name, target_pid, target_tgid, target_state,
           target_tracer_tgid, target_dumpable, target_uid, target_euid,
           target_is_chrome);
    pid_runlock();
}

static int ptrace_signal_valid(uint64 signo)
{
    return signo == 0 || (signo > 0 && signo <= NSIG);
}

static int ptrace_may_access(struct thread *target)
{
    struct thread_group *sender_tg;
    struct thread_group *target_tg;

    if (current == NULL || target == NULL || target == current)
        return 0;
    if (current->thread_group == target->thread_group)
        return 0;
    sender_tg = current->thread_group;
    target_tg = target->thread_group;
    if (sender_tg == NULL || target_tg == NULL || target_tg->is_kernel)
        return 0;
    if (sender_tg->euid == 0)
        return 1;
    if (__atomic_load_n(&target_tg->dumpable, __ATOMIC_ACQUIRE) == 0)
        return 0;
    return sender_tg->uid == target_tg->uid ||
           sender_tg->uid == target_tg->suid ||
           sender_tg->euid == target_tg->uid ||
           sender_tg->euid == target_tg->suid;
}

static int ptrace_find_target(int pid, struct thread **out)
{
    struct thread *target = NULL;

    if (out == NULL)
        return -EINVAL;
    *out = NULL;
    if (pid <= 0)
        return -ESRCH;
    if (get_pid_thread(pid, &target) != 0 || target == NULL)
        return -ESRCH;
    if (__thread_state_get(target) == THREAD_UNUSED ||
        __thread_state_get(target) == THREAD_ZOMBIE)
        return -ESRCH;
    *out = target;
    return 0;
}

static int ptrace_is_tracer(struct thread *target)
{
    if (current == NULL || target == NULL)
        return 0;
    return target->ptrace_tracer_tgid == thread_tgid(current);
}

static int ptrace_send_thread_signal(struct thread *target, int signo)
{
    ksiginfo_t info = {0};

    if (signo == 0)
        return 0;
    info.signo = signo;
    info.sender = current;
    info.info.si_pid = current != NULL ? thread_tgid(current) : 0;
    return __signal_send(target, &info);
}

static int ptrace_attach_common(int pid, int seize)
{
    struct thread *target = NULL;
    int parent_listed = 0;
    int ret;

    pid_wlock();
    ret = ptrace_find_target(pid, &target);
    if (ret != 0)
        goto out;
    if (!ptrace_may_access(target)) {
        ret = -EPERM;
        goto out;
    }
    if (target->ptrace_tracer != NULL) {
        ret = -EBUSY;
        goto out;
    }

    target->ptrace_tracer = current;
    target->ptrace_tracer_tgid = thread_tgid(current);
    target->ptrace_real_parent_pid =
        target->parent != NULL ? target->parent->pid : 0;
    target->ptrace_real_parent_seq =
        target->parent != NULL ? target->parent->pid_seq : 0;
    parent_listed =
        target->parent != NULL && !LIST_ENTRY_IS_DETACHED(&target->siblings);
    target->ptrace_real_parent_listed = parent_listed;
    target->ptrace_options = 0;

    if (target->parent != current) {
        if (parent_listed)
            detach_child(target->parent, target);
        else
            target->parent = NULL;
        attach_child(current, target);
    }
out:
    pid_wunlock();
    if (ret == 0 && !seize)
        ret = ptrace_send_thread_signal(target, SIGSTOP);
    return ret;
}

static int ptrace_set_options(int pid, uint64 options)
{
    struct thread *target = NULL;
    int ret;

    pid_rlock();
    ret = ptrace_find_target(pid, &target);
    if (ret == 0 && !ptrace_is_tracer(target))
        ret = -ESRCH;
    if (ret == 0)
        target->ptrace_options = options;
    pid_runlock();
    return ret;
}

static struct thread *ptrace_restore_parent_locked(struct thread *target)
{
    struct thread *parent = NULL;

    if (target == NULL)
        return NULL;
    if (target->ptrace_real_parent_pid > 0 &&
        get_pid_thread(target->ptrace_real_parent_pid, &parent) == 0 &&
        parent != NULL &&
        parent->pid_seq == target->ptrace_real_parent_seq &&
        __thread_state_get(parent) != THREAD_UNUSED &&
        __thread_state_get(parent) != THREAD_ZOMBIE) {
        return parent;
    }
    return __proctab_get_initproc();
}

static int ptrace_detach_common(int pid, uint64 signo)
{
    struct thread *target = NULL;
    struct thread *restore_parent;
    int wake_stopped = 0;
    int ret;

    if (!ptrace_signal_valid(signo))
        return -EINVAL;

    pid_wlock();
    ret = ptrace_find_target(pid, &target);
    if (ret != 0)
        goto out;
    if (!ptrace_is_tracer(target)) {
        ret = -ESRCH;
        goto out;
    }

    restore_parent = ptrace_restore_parent_locked(target);
    if (restore_parent == NULL) {
        ret = -ESRCH;
        goto out;
    }
    if (target->parent != restore_parent) {
        if (target->parent != NULL && !LIST_ENTRY_IS_DETACHED(&target->siblings))
            detach_child(target->parent, target);
        else
            target->parent = NULL;
        if (target->ptrace_real_parent_listed)
            attach_child(restore_parent, target);
        else
            target->parent = restore_parent;
    }

    target->ptrace_tracer = NULL;
    target->ptrace_tracer_tgid = 0;
    target->ptrace_real_parent_pid = 0;
    target->ptrace_real_parent_seq = 0;
    target->ptrace_real_parent_listed = 0;
    target->ptrace_options = 0;
    wake_stopped = THREAD_STOPPED(target);
out:
    pid_wunlock();

    if (ret == 0) {
        if (wake_stopped)
            scheduler_wakeup_stopped(target);
        ret = ptrace_send_thread_signal(target, (int)signo);
    }
    return ret;
}

static int ptrace_continue_common(int pid, uint64 signo)
{
    struct thread *target = NULL;
    int wake_stopped = 0;
    int ret;

    if (!ptrace_signal_valid(signo))
        return -EINVAL;

    pid_rlock();
    ret = ptrace_find_target(pid, &target);
    if (ret == 0 && !ptrace_is_tracer(target))
        ret = -ESRCH;
    if (ret == 0)
        wake_stopped = THREAD_STOPPED(target);
    pid_runlock();

    if (ret != 0)
        return ret;
    if (wake_stopped)
        scheduler_wakeup_stopped(target);
    return ptrace_send_thread_signal(target, (int)signo);
}

static void ptrace_fill_x86_regs(struct thread *target,
                                 struct ptrace_x86_user_regs *regs)
{
    memset(regs, 0, sizeof(*regs));
#ifdef __x86_64__
    if (target == NULL || target->trapframe == NULL)
        return;
    struct trapframe *tf = &target->trapframe->trapframe;
    regs->r15 = tf->r15;
    regs->r14 = tf->r14;
    regs->r13 = tf->r13;
    regs->r12 = tf->r12;
    regs->rbp = tf->rbp;
    regs->rbx = tf->rbx;
    regs->r11 = tf->r11;
    regs->r10 = tf->r10;
    regs->r9 = tf->r9;
    regs->r8 = tf->r8;
    regs->rax = tf->rax;
    regs->rcx = tf->rcx;
    regs->rdx = tf->rdx;
    regs->rsi = tf->rsi;
    regs->rdi = tf->rdi;
    regs->orig_rax = tf->rax;
    regs->rip = tf->rip;
    regs->cs = tf->cs;
    regs->eflags = tf->rflags;
    regs->rsp = tf->rsp;
    regs->ss = tf->ss;
    regs->fs_base = target->trapframe->tp;
    regs->gs_base = target->trapframe->user_gs_base;
#endif
}

static int ptrace_require_stopped(struct thread *target)
{
    if (!ptrace_is_tracer(target))
        return -ESRCH;
    if (!THREAD_STOPPED(target))
        return -EBUSY;
    return 0;
}

static int ptrace_getregs(int pid, uint64 data)
{
    struct thread *target = NULL;
    struct ptrace_x86_user_regs regs;
    int ret;

    if (data == 0)
        return -EFAULT;
    pid_rlock();
    ret = ptrace_find_target(pid, &target);
    if (ret == 0)
        ret = ptrace_require_stopped(target);
    if (ret == 0)
        ptrace_fill_x86_regs(target, &regs);
    pid_runlock();
    if (ret != 0)
        return ret;
    return either_copyout(1, data, &regs, sizeof(regs)) < 0 ? -EFAULT : 0;
}

static int ptrace_getregset(int pid, uint64 addr, uint64 data)
{
    struct ptrace_iovec iov;
    struct ptrace_x86_user_regs regs;
    uint8 fpregs[X86_FPU_LEGACY_SIZE];
    struct thread *target = NULL;
    uint64 copy_len;
    int ret;

    if (addr != NT_PRSTATUS && addr != NT_FPREGSET)
        return -EINVAL;
    if (data == 0)
        return -EFAULT;
    if (either_copyin(&iov, 1, data, sizeof(iov)) < 0)
        return -EFAULT;
    if (iov.iov_base == 0)
        return -EFAULT;

    pid_rlock();
    ret = ptrace_find_target(pid, &target);
    if (ret == 0)
        ret = ptrace_require_stopped(target);
    if (ret == 0) {
        if (addr == NT_PRSTATUS) {
            ptrace_fill_x86_regs(target, &regs);
        } else {
            memset(fpregs, 0, sizeof(fpregs));
            if (target->fpu_state != NULL) {
                if (mycpu()->fpu_owner_tid == target->pid &&
                    target->fpu_seq == mycpu()->fpu_seq)
                    fpu_save_state(target->fpu_state);
                memmove(fpregs, target->fpu_state, sizeof(fpregs));
            }
        }
    }
    pid_runlock();
    if (ret != 0)
        return ret;

    if (addr == NT_PRSTATUS)
        copy_len = iov.iov_len < sizeof(regs) ? iov.iov_len : sizeof(regs);
    else
        copy_len = iov.iov_len < sizeof(fpregs) ? iov.iov_len : sizeof(fpregs);
    if (copy_len != 0 &&
        either_copyout(1, iov.iov_base,
                       addr == NT_PRSTATUS ? (void *)&regs : (void *)fpregs,
                       copy_len) < 0)
        return -EFAULT;
    iov.iov_len = copy_len;
    return either_copyout(1, data, &iov, sizeof(iov)) < 0 ? -EFAULT : 0;
}

static int ptrace_peek(int request, int pid, uint64 addr, uint64 *out)
{
    struct ptrace_x86_user_regs regs;
    struct thread *target = NULL;
    vm_t *target_vm = NULL;
    uint64 word = 0;
    int ret;

    if (out == NULL)
        return -EINVAL;
    pid_rlock();
    ret = ptrace_find_target(pid, &target);
    if (ret == 0)
        ret = ptrace_require_stopped(target);
    if (ret == 0 && request == PTRACE_PEEKUSR) {
        if ((addr & (sizeof(uint64) - 1)) != 0 ||
            addr >= sizeof(struct ptrace_x86_user_regs)) {
            ret = -EIO;
        } else {
            ptrace_fill_x86_regs(target, &regs);
            memcpy(&word, ((char *)&regs) + addr, sizeof(word));
        }
    } else if (ret == 0) {
        target_vm = target->vm;
        if (target_vm == NULL)
            ret = -EIO;
    }
    pid_runlock();
    if (ret == 0 && request != PTRACE_PEEKUSR &&
        vm_copyin(target_vm, &word, addr, sizeof(word)) < 0)
        ret = -EIO;
    if (ret == 0)
        *out = word;
    return ret;
}

static int ptrace_poke(int pid, uint64 addr, uint64 data)
{
    struct thread *target = NULL;
    vm_t *target_vm = NULL;
    int ret;

    pid_rlock();
    ret = ptrace_find_target(pid, &target);
    if (ret == 0)
        ret = ptrace_require_stopped(target);
    if (ret == 0) {
        target_vm = target->vm;
        if (target_vm == NULL)
            ret = -EIO;
    }
    pid_runlock();
    if (ret == 0 && vm_copyout(target_vm, addr, &data, sizeof(data)) < 0)
        ret = -EIO;
    return ret;
}

uint64 sys_ptrace(void)
{
    int request;
    int pid;
    uint64 addr;
    uint64 data;
    struct thread *target = NULL;
    uint64 word = 0;
    int ret = 0;

    argint(0, &request);
    argint(1, &pid);
    argaddr(2, &addr);
    argaddr(3, &data);

    switch (request) {
    case PTRACE_TRACEME:
        if (current == NULL || current->parent == NULL) {
            ptrace_trace_result(request, pid, addr, data, -EPERM);
            return (uint64)-EPERM;
        }
        if (current->ptrace_tracer != NULL) {
            ptrace_trace_result(request, pid, addr, data, -EPERM);
            return (uint64)-EPERM;
        }
        current->ptrace_tracer = current->parent;
        current->ptrace_tracer_tgid = thread_tgid(current->parent);
        current->ptrace_real_parent_pid = current->parent->pid;
        current->ptrace_real_parent_seq = current->parent->pid_seq;
        current->ptrace_options = 0;
        ptrace_trace_result(request, pid, addr, data, 0);
        return 0;
    case PTRACE_ATTACH:
        ret = ptrace_attach_common(pid, 0);
        break;
    case PTRACE_SEIZE:
        if ((data & ~PTRACE_O_MASK) != 0)
            ret = -EINVAL;
        else {
            ret = ptrace_attach_common(pid, 1);
            if (ret == 0)
                ret = ptrace_set_options(pid, data);
        }
        break;
    case PTRACE_DETACH:
        ret = ptrace_detach_common(pid, data);
        break;
    case PTRACE_CONT:
        ret = ptrace_continue_common(pid, data);
        break;
    case PTRACE_INTERRUPT:
        pid_rlock();
        ret = ptrace_find_target(pid, &target);
        if (ret == 0 && !ptrace_is_tracer(target))
            ret = -ESRCH;
        pid_runlock();
        if (ret == 0)
            ret = ptrace_send_thread_signal(target, SIGSTOP);
        break;
    case PTRACE_SETOPTIONS:
        if ((data & ~PTRACE_O_MASK) != 0) {
            ret = -EINVAL;
            break;
        }
        pid_rlock();
        ret = ptrace_find_target(pid, &target);
        if (ret == 0 && !ptrace_is_tracer(target))
            ret = -ESRCH;
        if (ret == 0)
            target->ptrace_options = data;
        pid_runlock();
        break;
    case PTRACE_GETEVENTMSG:
        if (data == 0)
            ret = -EFAULT;
        else {
            uint64 zero = 0;
            ret = either_copyout(1, data, &zero, sizeof(zero)) < 0 ?
                -EFAULT : 0;
        }
        break;
    case PTRACE_GETSIGINFO:
        if (data == 0) {
            ret = -EFAULT;
        } else {
            char siginfo[128];
            memset(siginfo, 0, sizeof(siginfo));
            ret = either_copyout(1, data, siginfo, sizeof(siginfo)) < 0 ?
                -EFAULT : 0;
        }
        break;
    case PTRACE_GETREGSET:
        ret = ptrace_getregset(pid, addr, data);
        break;
    case 12: /* PTRACE_GETREGS, x86-64 */
        ret = ptrace_getregs(pid, data);
        break;
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA:
    case PTRACE_PEEKUSR:
        ret = ptrace_peek(request, pid, addr, &word);
        if (ret == 0) {
            ptrace_trace_result(request, pid, addr, data, 0);
            return word;
        }
        break;
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
        ret = ptrace_poke(pid, addr, data);
        break;
    default:
        if (ptrace_trace_caller())
            printf("ptrace: unsupported request=%d pid=%d addr=0x%lx data=0x%lx name=%s tgid=%d\n",
                   request, pid, addr, data, current ? current->name : "?",
                   current ? thread_tgid(current) : -1);
        ret = -EIO;
        break;
    }
    ptrace_trace_result(request, pid, addr, data, ret);
    return (uint64)ret;
}

#define LINUX_PRIVILEGED_STUB(name) \
    uint64 sys_##name(void) { return (uint64)-EPERM; }

#define LINUX_INVALID_STUB(name) \
    uint64 sys_##name(void) { return (uint64)-EINVAL; }

#define LINUX_NODEV_STUB(name) \
    uint64 sys_##name(void) { return (uint64)-ENODEV; }

#define SYSLOG_ACTION_CLOSE 0
#define SYSLOG_ACTION_OPEN 1
#define SYSLOG_ACTION_READ 2
#define SYSLOG_ACTION_READ_ALL 3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR 5
#define SYSLOG_ACTION_CONSOLE_OFF 6
#define SYSLOG_ACTION_CONSOLE_ON 7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD 9
#define SYSLOG_ACTION_SIZE_BUFFER 10

uint64 sys_syslog(void)
{
    int type;
    uint64 ubuf;
    int len;

    argint(0, &type);
    argaddr(1, &ubuf);
    argint(2, &len);

    if (!capable())
        return (uint64)-EPERM;

    switch (type) {
    case SYSLOG_ACTION_CLOSE:
    case SYSLOG_ACTION_OPEN:
    case SYSLOG_ACTION_CONSOLE_OFF:
    case SYSLOG_ACTION_CONSOLE_ON:
    case SYSLOG_ACTION_CONSOLE_LEVEL:
        return 0;
    case SYSLOG_ACTION_CLEAR:
        klog_clear();
        return 0;
    case SYSLOG_ACTION_SIZE_UNREAD:
        return klog_size();
    case SYSLOG_ACTION_SIZE_BUFFER:
        return klog_capacity();
    case SYSLOG_ACTION_READ:
    case SYSLOG_ACTION_READ_ALL:
    case SYSLOG_ACTION_READ_CLEAR:
        break;
    default:
        return (uint64)-EINVAL;
    }

    if (len < 0 || (len > 0 && ubuf == 0))
        return (uint64)-EINVAL;
    if (len == 0)
        return 0;

    size_t max = (size_t)len;
    if (max > klog_capacity())
        max = klog_capacity();

    char *buf = kvmalloc(max);
    if (buf == NULL)
        return (uint64)-ENOMEM;

    size_t n = klog_snapshot(buf, max, NULL);
    int ret = 0;
    if (n != 0 && either_copyout(1, ubuf, buf, n) < 0)
        ret = -EFAULT;
    kvfree(buf);

    if (ret < 0)
        return (uint64)ret;
    if (type == SYSLOG_ACTION_READ_CLEAR)
        klog_clear();
    return n;
}

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
LINUX_PRIVILEGED_STUB(finit_module)
LINUX_INVALID_STUB(seccomp)
LINUX_PRIVILEGED_STUB(kexec_file_load)
LINUX_PRIVILEGED_STUB(bpf)
LINUX_INVALID_STUB(userfaultfd)
LINUX_INVALID_STUB(io_pgetevents)
LINUX_INVALID_STUB(io_uring_setup)
LINUX_INVALID_STUB(io_uring_enter)
LINUX_INVALID_STUB(io_uring_register)
LINUX_PRIVILEGED_STUB(open_tree)
LINUX_PRIVILEGED_STUB(move_mount)
LINUX_INVALID_STUB(fsopen)
LINUX_INVALID_STUB(fsconfig)
LINUX_INVALID_STUB(fsmount)
LINUX_INVALID_STUB(fspick)
LINUX_INVALID_STUB(pidfd_getfd)
LINUX_INVALID_STUB(process_madvise)
LINUX_PRIVILEGED_STUB(mount_setattr)
LINUX_NODEV_STUB(quotactl_fd)
LINUX_INVALID_STUB(memfd_secret)
LINUX_INVALID_STUB(process_mrelease)
LINUX_INVALID_STUB(set_mempolicy_home_node)
LINUX_INVALID_STUB(cachestat)
LINUX_INVALID_STUB(map_shadow_stack)
LINUX_INVALID_STUB(statmount)
LINUX_INVALID_STUB(listmount)
LINUX_INVALID_STUB(lsm_get_self_attr)
LINUX_INVALID_STUB(lsm_set_self_attr)
LINUX_INVALID_STUB(lsm_list_modules)
