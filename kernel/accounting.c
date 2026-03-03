/**
 * @file accounting.c
 * @brief Per-process resource accounting + rlimit default initialisation
 *
 * Provides:
 *  - acct_init()           — zero all accounting counters
 *  - acct_format()         — render counters + rlimits as /proc text
 *  - rlimit_init_defaults()— fill the rlimit array with sane defaults
 *  - sys_prlimit64()       — prlimit64 syscall handler
 */

#include "accounting.h"
#include "resource.h"
#include "kstats.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/rq.h"
#include "proc/sched.h"
#include "defs.h"
#include "param.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include <mm/vm.h>
#include "dev/fdt.h"
#include "timer/timer.h"
#include <smp/percpu.h>

int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ------------------------------------------------------------------ */
/*  Resource limit defaults                                           */
/* ------------------------------------------------------------------ */

void rlimit_init_defaults(struct rlimit rlim[RLIMIT_NLIMITS]) {
    for (int i = 0; i < RLIMIT_NLIMITS; i++) {
        rlim[i].rlim_cur = RLIM_INFINITY;
        rlim[i].rlim_max = RLIM_INFINITY;
    }

    /* RLIMIT_NOFILE — match the compile-time NOFILE constant */
    rlim[RLIMIT_NOFILE].rlim_cur = NOFILE;
    rlim[RLIMIT_NOFILE].rlim_max = NOFILE;

    /* RLIMIT_NPROC — match the compile-time NR_THREAD constant */
    rlim[RLIMIT_NPROC].rlim_cur = NR_THREAD;
    rlim[RLIMIT_NPROC].rlim_max = NR_THREAD;

    /* RLIMIT_STACK — 8 MiB soft, unlimited hard */
    rlim[RLIMIT_STACK].rlim_cur = (uint64)MAXUSTACK * PAGE_SIZE;
    rlim[RLIMIT_STACK].rlim_max = RLIM_INFINITY;
}

/* ------------------------------------------------------------------ */
/*  Accounting initialisation                                         */
/* ------------------------------------------------------------------ */

void acct_init(struct thread_group *tg) {
    memset(&tg->acct, 0, sizeof(tg->acct));
}

/* ------------------------------------------------------------------ */
/*  Text formatting for /proc/<pid>/resources                         */
/* ------------------------------------------------------------------ */

/**
 * Helper: append a "key: value\n" line to buf.
 * Returns number of chars written (excluding NUL).
 */
static int fmt_u64(char *buf, int off, int sz, const char *key, uint64 val) {
    return snprintf(buf + off, sz - off > 0 ? sz - off : 0,
                    "%s: %lu\n", key, (unsigned long)val);
}

static int fmt_i64(char *buf, int off, int sz, const char *key, int64 val) {
    return snprintf(buf + off, sz - off > 0 ? sz - off : 0,
                    "%s: %ld\n", key, (long)val);
}

static int fmt_rlimit(char *buf, int off, int sz,
                      const char *name, struct rlimit *rl) {
    if (rl->rlim_cur == RLIM_INFINITY && rl->rlim_max == RLIM_INFINITY) {
        return snprintf(buf + off, sz - off > 0 ? sz - off : 0,
                        "%s: unlimited / unlimited\n", name);
    } else if (rl->rlim_max == RLIM_INFINITY) {
        return snprintf(buf + off, sz - off > 0 ? sz - off : 0,
                        "%s: %lu / unlimited\n",
                        name, (unsigned long)rl->rlim_cur);
    } else {
        return snprintf(buf + off, sz - off > 0 ? sz - off : 0,
                        "%s: %lu / %lu\n",
                        name, (unsigned long)rl->rlim_cur,
                        (unsigned long)rl->rlim_max);
    }
}

int acct_format(struct thread_group *tg, char *buf, int sz) {
    int off = 0;
    struct proc_acct *a = &tg->acct;

#define EMIT_U64(field) do {                                              \
    int n = fmt_u64(buf, off, sz, #field,                                 \
                    __atomic_load_n(&a->field, __ATOMIC_RELAXED));        \
    if (n > 0) off += n;                                                  \
} while (0)

#define EMIT_I64(field) do {                                              \
    int n = fmt_i64(buf, off, sz, #field,                                 \
                    __atomic_load_n(&a->field, __ATOMIC_RELAXED));        \
    if (n > 0) off += n;                                                  \
} while (0)

    /* FS */
    EMIT_U64(fs_opens);
    EMIT_U64(fs_closes);
    EMIT_U64(fs_bytes_read);
    EMIT_U64(fs_bytes_written);
    EMIT_U64(fs_creates);
    EMIT_U64(fs_deletes);
    EMIT_U64(fs_renames);
    EMIT_U64(fs_links);
    EMIT_U64(fs_chdirs);
    EMIT_U64(fs_mounts);

    /* BIO */
    EMIT_U64(bio_reads);
    EMIT_U64(bio_writes);

    /* Net */
    EMIT_U64(net_sockets);
    EMIT_U64(net_connects);
    EMIT_U64(net_accepts);
    EMIT_U64(net_bytes_sent);
    EMIT_U64(net_bytes_recv);

    /* MM */
    EMIT_U64(mm_mmap_count);
    EMIT_U64(mm_munmap_count);
    EMIT_I64(mm_brk_delta);
    EMIT_U64(mm_peak_vm);

    /* Sched */
    EMIT_U64(sched_forks);
    EMIT_U64(sched_execs);
    EMIT_U64(sched_exits);

#undef EMIT_U64
#undef EMIT_I64

    /* rlimits */
    static const char *rlimit_names[RLIMIT_NLIMITS] = {
        "rlimit_cpu",
        "rlimit_fsize",
        "rlimit_data",
        "rlimit_stack",
        "rlimit_core",
        "rlimit_rss",
        "rlimit_nproc",
        "rlimit_nofile",
        "rlimit_memlock",
        "rlimit_as",
    };

    for (int i = 0; i < RLIMIT_NLIMITS; i++) {
        int n = fmt_rlimit(buf, off, sz, rlimit_names[i], &tg->rlim[i]);
        if (n > 0)
            off += n;
    }

    return off;
}

/* ------------------------------------------------------------------ */
/*  sys_prlimit64 — get/set resource limits                           */
/* ------------------------------------------------------------------ */

/*
 * prlimit64(pid, resource, new_limit_uaddr, old_limit_uaddr)
 *
 *   a0 = pid       (0 = self)
 *   a1 = resource   (RLIMIT_*)
 *   a2 = new_limit  (user pointer, or 0 to skip)
 *   a3 = old_limit  (user pointer, or 0 to skip)
 *
 * Returns 0 on success, negative errno on failure.
 */
uint64 sys_prlimit64(void) {
    int pid, resource;
    uint64 new_addr, old_addr;

    argint(0, &pid);
    argint(1, &resource);
    argaddr(2, &new_addr);
    argaddr(3, &old_addr);

    if (resource < 0 || resource >= RLIMIT_NLIMITS)
        return -EINVAL;

    struct thread_group *tg;

    if (pid == 0) {
        tg = current->thread_group;
    } else {
        /* Look up the target process by tgid */
        rcu_read_lock();
        struct thread *target = NULL;
        get_pid_thread(pid, &target);
        if (target == NULL || target->tgid != pid ||
            target->pid != target->tgid) {
            rcu_read_unlock();
            return -ESRCH;
        }
        tg = target->thread_group;
        rcu_read_unlock();
        if (tg == NULL)
            return -ESRCH;
    }

    /* Return the old limit to user-space */
    if (old_addr != 0) {
        if (vm_copyout(current->vm, old_addr, (char *)&tg->rlim[resource],
                       sizeof(struct rlimit)) < 0)
            return -EFAULT;
    }

    /* Set a new limit from user-space */
    if (new_addr != 0) {
        struct rlimit new_rl;
        if (vm_copyin(current->vm, (char *)&new_rl, new_addr,
                      sizeof(struct rlimit)) < 0)
            return -EFAULT;

        /* Soft limit must not exceed hard limit */
        if (new_rl.rlim_cur > new_rl.rlim_max)
            return -EINVAL;

        /* Unprivileged process cannot raise the hard limit */
        if (new_rl.rlim_max > tg->rlim[resource].rlim_max)
            return -EPERM;

        tg->rlim[resource] = new_rl;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  System-wide kernel statistics                                     */
/* ------------------------------------------------------------------ */

void kstats_collect(struct kstats *ks) {
    memset(ks, 0, sizeof(*ks));

    ks->uptime_ms = (uint64)get_jiffs();
    ks->ncpus = platform.ncpu;
    ks->timebase_freq = __timebase_frequency;
    ks->timestamp = r_time();
    if (ks->ncpus > KSTATS_MAX_CPUS)
        ks->ncpus = KSTATS_MAX_CPUS;

    /* System-wide load averages (filled by sched.c calc_load). */
    {
        uint64 loads[3];
        get_avenrun(loads);
        ks->load_avg_1s  = loads[0];
        ks->load_avg_5s  = loads[1];
        ks->load_avg_16s = loads[2];
    }

    for (int cpu = 0; cpu < ks->ncpus; cpu++) {
        ks->cpu[cpu].idle = rq_cpu_is_idle(cpu) ? 1 : 0;

        /* Sum nr_running + load_avg + PELT across EEVDF priority levels */
        uint32 total_nr = 0;
        uint64 total_load = 0;
        uint64 total_cpu_util = 0;
        uint64 total_cpu_load = 0;
        for (int cls = EEVDF_MAJOR_PRIORITY_START;
             cls < EEVDF_MAJOR_PRIORITY_LIMIT; cls++) {
            struct rq *rq = get_rq_for_cpu(cls, cpu);
            if (IS_ERR_OR_NULL(rq))
                continue;
            struct eevdf_rq *erq = container_of(rq, struct eevdf_rq, rq);
            total_nr += erq->nr_running;
            total_load += erq->load_avg;
            total_cpu_util += erq->cpu_util;
            total_cpu_load += erq->cpu_load;
        }
        ks->cpu[cpu].nr_running = total_nr;
        ks->cpu[cpu].load_avg = total_load;
        ks->cpu[cpu].cpu_util = total_cpu_util;
        ks->cpu[cpu].cpu_load = total_cpu_load;
        /* Scheduler-agnostic per-CPU busy/total ticks + 1s util. */
        ks->cpu[cpu].busy_ticks = cpus[cpu].busy_ticks;
        ks->cpu[cpu].total_ticks = cpus[cpu].total_ticks;
        ks->cpu[cpu].util_1s = cpus[cpu].util_1s;
    }

    ks->bio_reads = __atomic_load_n(&g_bio_reads, __ATOMIC_RELAXED);
    ks->bio_writes = __atomic_load_n(&g_bio_writes, __ATOMIC_RELAXED);
    ks->bio_read_bytes = __atomic_load_n(&g_bio_read_bytes, __ATOMIC_RELAXED);
    ks->bio_write_bytes = __atomic_load_n(&g_bio_write_bytes, __ATOMIC_RELAXED);

    ks->net_tx_packets = __atomic_load_n(&g_net_tx_packets, __ATOMIC_RELAXED);
    ks->net_tx_bytes = __atomic_load_n(&g_net_tx_bytes, __ATOMIC_RELAXED);
    ks->net_rx_packets = __atomic_load_n(&g_net_rx_packets, __ATOMIC_RELAXED);
    ks->net_rx_bytes = __atomic_load_n(&g_net_rx_bytes, __ATOMIC_RELAXED);
}

/*
 * sys_kstats(uaddr)
 *
 *   a0 = user pointer to struct kstats
 *
 * Returns 0 on success, negative errno on failure.
 */
uint64 sys_kstats(void) {
    uint64 uaddr;
    argaddr(0, &uaddr);

    if (uaddr == 0)
        return -EINVAL;

    struct kstats ks;
    kstats_collect(&ks);

    if (vm_copyout(current->vm, uaddr, (char *)&ks, sizeof(ks)) < 0)
        return -EFAULT;

    return 0;
}
