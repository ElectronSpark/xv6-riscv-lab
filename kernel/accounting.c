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

uint64 g_vfs_lookup_calls;
uint64 g_vfs_lookup_dcache_hits;
uint64 g_vfs_lookup_negative_hits;
uint64 g_vfs_lookup_cache_misses;
uint64 g_vfs_lookup_driver_calls;
uint64 g_vfs_lookup_driver_ticks;
uint64 g_vfs_dentry_inode_calls;
uint64 g_vfs_dentry_inode_ticks;
uint64 g_vfs_dentry_inode_retries;
uint64 g_vfs_dentry_inode_self_hits;
uint64 g_vfs_dentry_inode_rlock_calls;
uint64 g_vfs_dentry_inode_rlock_ticks;
uint64 g_vfs_dentry_inode_upgrade_calls;
uint64 g_vfs_dentry_inode_upgrade_ticks;
uint64 g_vfs_inode_cache_calls;
uint64 g_vfs_inode_cache_hits;
uint64 g_vfs_inode_cache_misses;
uint64 g_vfs_inode_cache_eagain;
uint64 g_vfs_inode_cache_miss_hash;
uint64 g_vfs_inode_cache_miss_revive_without_wlock;
uint64 g_vfs_inode_cache_miss_dying;
uint64 g_vfs_inode_cache_miss_invalid_destroying;
uint64 g_vfs_inode_cache_read_revive_attempts;
uint64 g_vfs_inode_cache_read_revive_success;
uint64 g_vfs_inode_cache_read_revive_lock_fail;
uint64 g_vfs_inode_cache_read_revive_stale;
uint64 g_vfs_inode_cache_ticks;
uint64 g_vfs_inode_load_calls;
uint64 g_vfs_inode_load_success;
uint64 g_vfs_inode_load_ticks;

uint64 g_vm_copyin_calls;
uint64 g_vm_copyout_calls;
uint64 g_vm_copyin_bytes;
uint64 g_vm_copyout_bytes;
uint64 g_vm_vma_validate_calls;
uint64 g_vm_vma_validate_ticks;
uint64 g_vm_file_faults;
uint64 g_vm_validate_batch_ticks;
uint64 g_vm_validate_fallback_ticks;
uint64 g_vm_validate_hugepage_ticks;
uint64 g_vm_validate_pte_check_ticks;
uint64 g_vm_copyin_ticks;
uint64 g_vm_copyout_ticks;

uint64 g_ext4_pcache_read_page_calls;
uint64 g_ext4_pcache_pages_filled;
uint64 g_ext4_pcache_readahead_pages;
uint64 g_ext4_pcache_read_page_ticks;
uint64 g_ext4_fault_calls;
uint64 g_ext4_fault_zero_copy;
uint64 g_ext4_fault_partial_copy;
uint64 g_ext4_fault_ticks;
uint64 g_ext4_lookup_calls;
uint64 g_ext4_lookup_lock_wait_ticks;
uint64 g_ext4_lookup_lock_hold_ticks;
uint64 g_ext4_lookup_parent_ref_ticks;
uint64 g_ext4_lookup_dir_find_ticks;
uint64 g_ext4_lookup_found;
uint64 g_ext4_lookup_enoent;
uint64 g_ext4_lookup_errors;

uint64 g_sys_open_calls;
uint64 g_sys_open_ticks;
uint64 g_sys_fstat_calls;
uint64 g_sys_fstat_ticks;
uint64 g_sys_lseek_calls;
uint64 g_sys_lseek_ticks;
uint64 g_sys_pread64_calls;
uint64 g_sys_pread64_ticks;
uint64 g_sys_openat_calls;
uint64 g_sys_openat_ticks;
uint64 g_sys_fstatat_calls;
uint64 g_sys_fstatat_ticks;
uint64 g_sys_faccessat_calls;
uint64 g_sys_faccessat_ticks;
uint64 g_sys_read_calls;
uint64 g_sys_read_ticks;
uint64 g_sys_readv_calls;
uint64 g_sys_readv_ticks;
uint64 g_sys_getdents_calls;
uint64 g_sys_getdents_ticks;
uint64 g_sys_readlinkat_calls;
uint64 g_sys_readlinkat_ticks;
uint64 g_sys_mmap_calls;
uint64 g_sys_mmap_ticks;
uint64 g_sys_munmap_calls;
uint64 g_sys_munmap_ticks;
uint64 g_vm_munmap_pages_freed;
uint64 g_vm_munmap_pte_walk_ticks;
uint64 g_vm_munmap_page_release_ticks;
uint64 g_vm_munmap_anon_pages;
uint64 g_sys_mprotect_calls;
uint64 g_sys_mprotect_ticks;
uint64 g_sys_brk_calls;
uint64 g_sys_brk_ticks;
uint64 g_sys_clock_gettime_calls;
uint64 g_sys_clock_gettime_ticks;
uint64 g_sys_clock_gettime_monotonic_calls;
uint64 g_sys_clock_gettime_monotonic_ticks;
uint64 g_sys_clock_gettime_monotonic_coarse_calls;
uint64 g_sys_clock_gettime_monotonic_coarse_ticks;
uint64 g_sys_clock_gettime_realtime_calls;
uint64 g_sys_clock_gettime_realtime_ticks;
uint64 g_sys_clock_gettime_process_calls;
uint64 g_sys_clock_gettime_process_ticks;
uint64 g_sys_clock_gettime_thread_calls;
uint64 g_sys_clock_gettime_thread_ticks;
uint64 g_sys_clock_gettime_other_calls;
uint64 g_sys_clock_gettime_other_ticks;
uint64 g_sys_gettimeofday_calls;
uint64 g_sys_gettimeofday_ticks;
uint64 g_sys_getrandom_calls;
uint64 g_sys_getrandom_ticks;
uint64 g_exec_calls;
uint64 g_exec_ticks;
uint64 g_sys_openat_path_copy_calls;
uint64 g_sys_openat_path_copy_ticks;
uint64 g_sys_openat_dirfd_calls;
uint64 g_sys_openat_dirfd_ticks;
uint64 g_sys_openat_lookup_calls;
uint64 g_sys_openat_lookup_ticks;
uint64 g_sys_openat_fileopen_calls;
uint64 g_sys_openat_fileopen_ticks;
uint64 g_sys_openat_fdalloc_calls;
uint64 g_sys_openat_fdalloc_ticks;
uint64 g_sys_poll_calls;
uint64 g_sys_poll_ticks;
uint64 g_sys_ppoll_calls;
uint64 g_sys_ppoll_ticks;
uint64 g_sys_poll_blocking_calls;
uint64 g_sys_poll_blocking_ticks;
uint64 g_sys_ioctl_calls;
uint64 g_sys_ioctl_ticks;
uint64 g_sys_ioctl_tty_tcgets_calls;
uint64 g_sys_ioctl_tty_tcgets_ticks;
uint64 g_sys_ioctl_tty_tcsets_calls;
uint64 g_sys_ioctl_tty_tcsets_ticks;
uint64 g_sys_ioctl_tty_winsz_calls;
uint64 g_sys_ioctl_tty_winsz_ticks;
uint64 g_sys_ioctl_tty_pgrp_calls;
uint64 g_sys_ioctl_tty_pgrp_ticks;
uint64 g_sys_ioctl_tty_ptmx_calls;
uint64 g_sys_ioctl_tty_ptmx_ticks;
uint64 g_sys_ioctl_tty_ctty_calls;
uint64 g_sys_ioctl_tty_ctty_ticks;
uint64 g_sys_futex_calls;
uint64 g_sys_futex_ticks;
uint64 g_sys_futex_wait_calls;
uint64 g_sys_futex_wait_ticks;
uint64 g_sys_futex_wake_calls;
uint64 g_sys_futex_wake_ticks;
uint64 g_sys_poll_wait_unix_calls;
uint64 g_sys_poll_wait_unix_ticks;
uint64 g_sys_poll_wait_eventfd_calls;
uint64 g_sys_poll_wait_eventfd_ticks;
uint64 g_sys_poll_wait_pipe_calls;
uint64 g_sys_poll_wait_pipe_ticks;
uint64 g_sys_poll_wait_other_calls;
uint64 g_sys_poll_wait_other_ticks;
uint64 g_sys_poll_wait_notify_calls;
uint64 g_sys_poll_wait_notify_ticks;
uint64 g_sys_poll_wait_rescan_calls;
uint64 g_sys_poll_wait_rescan_ticks;
uint64 g_sys_poll_wait_ready_calls;
uint64 g_sys_poll_wait_ready_ticks;
uint64 g_sys_poll_wait_timeout_calls;
uint64 g_sys_poll_wait_timeout_ticks;
uint64 g_konsole_prepty_poll_total_calls;
uint64 g_konsole_prepty_poll_total_ticks;
uint64 g_konsole_prepty_poll_wayland_calls;
uint64 g_konsole_prepty_poll_wayland_ticks;
uint64 g_konsole_prepty_poll_qdbus_calls;
uint64 g_konsole_prepty_poll_qdbus_ticks;
uint64 g_konsole_prepty_poll_unix_other_calls;
uint64 g_konsole_prepty_poll_unix_other_ticks;
uint64 g_konsole_prepty_poll_eventfd_calls;
uint64 g_konsole_prepty_poll_eventfd_ticks;
uint64 g_konsole_prepty_poll_pipe_calls;
uint64 g_konsole_prepty_poll_pipe_ticks;
uint64 g_konsole_prepty_poll_other_calls;
uint64 g_konsole_prepty_poll_other_ticks;
uint64 g_konsole_prepty_poll_ready_calls;
uint64 g_konsole_prepty_poll_ready_ticks;
uint64 g_konsole_prepty_poll_timeout_calls;
uint64 g_konsole_prepty_poll_timeout_ticks;
uint64 g_konsole_prepty_futex_wait_calls;
uint64 g_konsole_prepty_futex_wait_ticks;
uint64 g_konsole_prepty_futex_woken_calls;
uint64 g_konsole_prepty_futex_woken_ticks;
uint64 g_konsole_prepty_futex_timeout_calls;
uint64 g_konsole_prepty_futex_timeout_ticks;
uint64 g_konsole_prepty_futex_signal_calls;
uint64 g_konsole_prepty_futex_signal_ticks;
uint64 g_konsole_prepty_futex_other_calls;
uint64 g_konsole_prepty_futex_other_ticks;
uint64 g_konsole_prepty_pty_seen;
uint64 g_konsole_prepty_poll_wayland_pipe_calls;
uint64 g_konsole_prepty_poll_wayland_pipe_ticks;
uint64 g_konsole_prepty_poll_wayland_eventfd_calls;
uint64 g_konsole_prepty_poll_wayland_eventfd_ticks;
uint64 g_konsole_prepty_poll_qdbus_pipe_calls;
uint64 g_konsole_prepty_poll_qdbus_pipe_ticks;
uint64 g_konsole_prepty_poll_qdbus_eventfd_calls;
uint64 g_konsole_prepty_poll_qdbus_eventfd_ticks;
uint64 g_konsole_prepty_poll_unix_other_pipe_calls;
uint64 g_konsole_prepty_poll_unix_other_pipe_ticks;
uint64 g_konsole_prepty_poll_unix_other_eventfd_calls;
uint64 g_konsole_prepty_poll_unix_other_eventfd_ticks;
uint64 g_konsole_prepty_poll_eventfd_pipe_calls;
uint64 g_konsole_prepty_poll_eventfd_pipe_ticks;
uint64 g_konsole_prepty_poll_wayland_only_calls;
uint64 g_konsole_prepty_poll_wayland_only_ticks;
uint64 g_konsole_prepty_poll_qdbus_only_calls;
uint64 g_konsole_prepty_poll_qdbus_only_ticks;
uint64 g_konsole_prepty_poll_unix_other_only_calls;
uint64 g_konsole_prepty_poll_unix_other_only_ticks;
uint64 g_konsole_prepty_poll_eventfd_only_calls;
uint64 g_konsole_prepty_poll_eventfd_only_ticks;
uint64 g_konsole_prepty_poll_pipe_only_calls;
uint64 g_konsole_prepty_poll_pipe_only_ticks;
uint64 g_konsole_prepty_poll_kqueue_wake_calls;
uint64 g_konsole_prepty_poll_kqueue_wake_ticks;
uint64 g_konsole_prepty_poll_timed_rescan_calls;
uint64 g_konsole_prepty_poll_timed_rescan_ticks;
uint64 g_konsole_prepty_poll_rescan_ready_calls;
uint64 g_konsole_prepty_poll_rescan_ready_ticks;
uint64 g_konsole_prepty_poll_event_ready_calls;
uint64 g_konsole_prepty_poll_event_ready_ticks;
uint64 g_konsole_prepty_poll_ready_wayland_calls;
uint64 g_konsole_prepty_poll_ready_wayland_ticks;
uint64 g_konsole_prepty_poll_ready_qdbus_calls;
uint64 g_konsole_prepty_poll_ready_qdbus_ticks;
uint64 g_konsole_prepty_poll_ready_unix_other_calls;
uint64 g_konsole_prepty_poll_ready_unix_other_ticks;
uint64 g_konsole_prepty_poll_ready_eventfd_calls;
uint64 g_konsole_prepty_poll_ready_eventfd_ticks;
uint64 g_konsole_prepty_poll_ready_pipe_calls;
uint64 g_konsole_prepty_poll_ready_pipe_ticks;
uint64 g_konsole_prepty_poll_rescan_ready_wayland_calls;
uint64 g_konsole_prepty_poll_rescan_ready_wayland_ticks;
uint64 g_konsole_prepty_poll_rescan_ready_qdbus_calls;
uint64 g_konsole_prepty_poll_rescan_ready_qdbus_ticks;
uint64 g_konsole_prepty_poll_rescan_ready_unix_other_calls;
uint64 g_konsole_prepty_poll_rescan_ready_unix_other_ticks;
uint64 g_konsole_prepty_poll_rescan_ready_eventfd_calls;
uint64 g_konsole_prepty_poll_rescan_ready_eventfd_ticks;
uint64 g_konsole_prepty_poll_rescan_ready_pipe_calls;
uint64 g_konsole_prepty_poll_rescan_ready_pipe_ticks;
int g_kstats_profile_enabled;

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

    /*
     * RLIMIT_NOFILE: imported Linux daemons commonly raise the hard limit
     * during startup.  The fd allocator still clips to NOFILE, so the hard
     * limit satisfies the Linux ABI probe without growing each fd table.
     */
    rlim[RLIMIT_NOFILE].rlim_cur = NOFILE;
    rlim[RLIMIT_NOFILE].rlim_max = 1024 * 1024;

    /* RLIMIT_NPROC — match the compile-time NR_THREAD constant */
    rlim[RLIMIT_NPROC].rlim_cur = NR_THREAD;
    rlim[RLIMIT_NPROC].rlim_max = NR_THREAD;

    /* RLIMIT_STACK — 8 MiB soft, unlimited hard */
    rlim[RLIMIT_STACK].rlim_cur = (uint64)MAXUSTACK * PAGE_SIZE;
    rlim[RLIMIT_STACK].rlim_max = RLIM_INFINITY;

    /*
     * Match the common Linux desktop/session default: crash diagnostics are
     * logged, but a faulting GUI process does not synchronously write a
     * multi-hundred-MiB core file unless userland explicitly opts in.
     */
    rlim[RLIMIT_CORE].rlim_cur = 0;
    rlim[RLIMIT_CORE].rlim_max = RLIM_INFINITY;

    /*
     * Linux GUI runtimes probe the post-RLIMIT_AS resources even when xv6 does
     * not enforce every one yet.  Return Linux-shaped defaults instead of
     * EINVAL so feature probes and sandbox setup follow the normal path.
     */
    rlim[RLIMIT_SIGPENDING].rlim_cur = NR_THREAD;
    rlim[RLIMIT_SIGPENDING].rlim_max = NR_THREAD;
    rlim[RLIMIT_MSGQUEUE].rlim_cur = 819200;
    rlim[RLIMIT_MSGQUEUE].rlim_max = 819200;
    rlim[RLIMIT_NICE].rlim_cur = 0;
    rlim[RLIMIT_NICE].rlim_max = 0;
    rlim[RLIMIT_RTPRIO].rlim_cur = 0;
    rlim[RLIMIT_RTPRIO].rlim_max = 0;
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
    EMIT_U64(mm_rss_pages);
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
        "rlimit_locks",
        "rlimit_sigpending",
        "rlimit_msgqueue",
        "rlimit_nice",
        "rlimit_rtprio",
        "rlimit_rttime",
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
    int need_put = 0;

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
        /* Look up the target process by TGID and pin its thread_group. */
        rcu_read_lock();
        struct thread *target = NULL;
        get_pid_thread(pid, &target);
        if (target == NULL || target->tgid != pid ||
            target->pid != target->tgid) {
            rcu_read_unlock();
            return -ESRCH;
        }
        tg = target->thread_group;
        if (tg != NULL) {
            thread_group_get(tg);
            need_put = 1;
        }
        rcu_read_unlock();
        if (tg == NULL)
            return -ESRCH;
    }

    /* Return the old limit to user-space */
    if (old_addr != 0) {
        if (vm_copyout(current->vm, old_addr, (char *)&tg->rlim[resource],
                       sizeof(struct rlimit)) < 0) {
            if (need_put)
                thread_group_put(tg);
            return -EFAULT;
        }
    }

    /* Set a new limit from user-space */
    if (new_addr != 0) {
        struct rlimit new_rl;
        if (vm_copyin(current->vm, (char *)&new_rl, new_addr,
                      sizeof(struct rlimit)) < 0) {
            if (need_put)
                thread_group_put(tg);
            return -EFAULT;
        }

        /* Soft limit must not exceed hard limit */
        if (new_rl.rlim_cur > new_rl.rlim_max) {
            if (need_put)
                thread_group_put(tg);
            return -EINVAL;
        }

        /* Unprivileged process cannot raise the hard limit */
        if (new_rl.rlim_max > tg->rlim[resource].rlim_max) {
            if (need_put)
                thread_group_put(tg);
            return -EPERM;
        }

        tg->rlim[resource] = new_rl;
    }

    if (need_put)
        thread_group_put(tg);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sys_getrlimit / sys_setrlimit — legacy rlimit syscalls            */
/* ------------------------------------------------------------------ */

/*
 * getrlimit(resource, old_limit_uaddr)
 *   a0 = resource (RLIMIT_*)
 *   a1 = old_limit (user pointer)
 */
uint64 sys_getrlimit(void) {
    int resource;
    uint64 old_addr;

    argint(0, &resource);
    argaddr(1, &old_addr);

    if (resource < 0 || resource >= RLIMIT_NLIMITS)
        return -EINVAL;

    struct thread_group *tg = current->thread_group;

    if (old_addr != 0) {
        if (vm_copyout(current->vm, old_addr, (char *)&tg->rlim[resource],
                       sizeof(struct rlimit)) < 0)
            return -EFAULT;
    }

    return 0;
}

/*
 * setrlimit(resource, new_limit_uaddr)
 *   a0 = resource (RLIMIT_*)
 *   a1 = new_limit (user pointer)
 */
uint64 sys_setrlimit(void) {
    int resource;
    uint64 new_addr;

    argint(0, &resource);
    argaddr(1, &new_addr);

    if (resource < 0 || resource >= RLIMIT_NLIMITS)
        return -EINVAL;

    struct thread_group *tg = current->thread_group;

    if (new_addr != 0) {
        struct rlimit new_rl;
        if (vm_copyin(current->vm, (char *)&new_rl, new_addr,
                      sizeof(struct rlimit)) < 0)
            return -EFAULT;

        if (new_rl.rlim_cur > new_rl.rlim_max)
            return -EINVAL;

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

    ks->bio_reads = g_bio_reads;
    ks->bio_writes = g_bio_writes;
    ks->bio_read_bytes = g_bio_read_bytes;
    ks->bio_write_bytes = g_bio_write_bytes;

    ks->net_tx_packets = g_net_tx_packets;
    ks->net_tx_bytes = g_net_tx_bytes;
    ks->net_rx_packets = g_net_rx_packets;
    ks->net_rx_bytes = g_net_rx_bytes;

    ks->vfs_lookup_calls =
        g_vfs_lookup_calls;
    ks->vfs_lookup_dcache_hits =
        g_vfs_lookup_dcache_hits;
    ks->vfs_lookup_negative_hits =
        g_vfs_lookup_negative_hits;
    ks->vfs_lookup_cache_misses =
        g_vfs_lookup_cache_misses;
    ks->vfs_lookup_driver_calls =
        g_vfs_lookup_driver_calls;
    ks->vfs_lookup_driver_ticks =
        g_vfs_lookup_driver_ticks;
    ks->vfs_dentry_inode_calls =
        g_vfs_dentry_inode_calls;
    ks->vfs_dentry_inode_ticks =
        g_vfs_dentry_inode_ticks;
    ks->vfs_dentry_inode_retries =
        g_vfs_dentry_inode_retries;
    ks->vfs_dentry_inode_self_hits =
        g_vfs_dentry_inode_self_hits;
    ks->vfs_dentry_inode_rlock_calls =
        g_vfs_dentry_inode_rlock_calls;
    ks->vfs_dentry_inode_rlock_ticks =
        g_vfs_dentry_inode_rlock_ticks;
    ks->vfs_dentry_inode_upgrade_calls =
        g_vfs_dentry_inode_upgrade_calls;
    ks->vfs_dentry_inode_upgrade_ticks =
        g_vfs_dentry_inode_upgrade_ticks;
    ks->vfs_inode_cache_calls =
        g_vfs_inode_cache_calls;
    ks->vfs_inode_cache_hits =
        g_vfs_inode_cache_hits;
    ks->vfs_inode_cache_misses =
        g_vfs_inode_cache_misses;
    ks->vfs_inode_cache_eagain =
        g_vfs_inode_cache_eagain;
    ks->vfs_inode_cache_miss_hash =
        g_vfs_inode_cache_miss_hash;
    ks->vfs_inode_cache_miss_revive_without_wlock =
        g_vfs_inode_cache_miss_revive_without_wlock;
    ks->vfs_inode_cache_miss_dying =
        g_vfs_inode_cache_miss_dying;
    ks->vfs_inode_cache_miss_invalid_destroying =
        g_vfs_inode_cache_miss_invalid_destroying;
    ks->vfs_inode_cache_read_revive_attempts =
        g_vfs_inode_cache_read_revive_attempts;
    ks->vfs_inode_cache_read_revive_success =
        g_vfs_inode_cache_read_revive_success;
    ks->vfs_inode_cache_read_revive_lock_fail =
        g_vfs_inode_cache_read_revive_lock_fail;
    ks->vfs_inode_cache_read_revive_stale =
        g_vfs_inode_cache_read_revive_stale;
    ks->vfs_inode_cache_ticks =
        g_vfs_inode_cache_ticks;
    ks->vfs_inode_load_calls =
        g_vfs_inode_load_calls;
    ks->vfs_inode_load_success =
        g_vfs_inode_load_success;
    ks->vfs_inode_load_ticks =
        g_vfs_inode_load_ticks;

    ks->vm_copyin_calls =
        g_vm_copyin_calls;
    ks->vm_copyout_calls =
        g_vm_copyout_calls;
    ks->vm_copyin_bytes =
        g_vm_copyin_bytes;
    ks->vm_copyout_bytes =
        g_vm_copyout_bytes;
    ks->vm_copyout_fast_hits =
        g_vm_copyout_fast_hits;
    ks->vm_copyout_fast_bytes =
        g_vm_copyout_fast_bytes;
    ks->vm_copyin_fast_hits =
        g_vm_copyin_fast_hits;
    ks->vm_copyin_fast_bytes =
        g_vm_copyin_fast_bytes;
    ks->vm_copyout_present_skip_hits =
        g_vm_copyout_present_skip_hits;
    ks->vm_copyout_present_skip_bytes =
        g_vm_copyout_present_skip_bytes;
    ks->vm_copyin_present_skip_hits =
        g_vm_copyin_present_skip_hits;
    ks->vm_copyin_present_skip_bytes =
        g_vm_copyin_present_skip_bytes;
    ks->vm_vma_validate_calls =
        g_vm_vma_validate_calls;
    ks->vm_vma_validate_ticks =
        g_vm_vma_validate_ticks;
    ks->vm_file_faults =
        g_vm_file_faults;
    ks->vm_validate_batch_ticks =
        g_vm_validate_batch_ticks;
    ks->vm_validate_fallback_ticks =
        g_vm_validate_fallback_ticks;
    ks->vm_validate_hugepage_ticks =
        g_vm_validate_hugepage_ticks;
    ks->vm_validate_pte_check_ticks =
        g_vm_validate_pte_check_ticks;
    ks->vm_copyin_ticks =
        g_vm_copyin_ticks;
    ks->vm_copyout_ticks =
        g_vm_copyout_ticks;

    ks->ext4_pcache_read_page_calls =
        g_ext4_pcache_read_page_calls;
    ks->ext4_pcache_pages_filled =
        g_ext4_pcache_pages_filled;
    ks->ext4_pcache_readahead_pages =
        g_ext4_pcache_readahead_pages;
    ks->ext4_pcache_read_page_ticks =
        g_ext4_pcache_read_page_ticks;
    ks->ext4_fault_calls =
        g_ext4_fault_calls;
    ks->ext4_fault_zero_copy =
        g_ext4_fault_zero_copy;
    ks->ext4_fault_partial_copy =
        g_ext4_fault_partial_copy;
    ks->ext4_fault_ticks =
        g_ext4_fault_ticks;
    ks->ext4_lookup_calls =
        g_ext4_lookup_calls;
    ks->ext4_lookup_lock_wait_ticks =
        g_ext4_lookup_lock_wait_ticks;
    ks->ext4_lookup_lock_hold_ticks =
        g_ext4_lookup_lock_hold_ticks;
    ks->ext4_lookup_parent_ref_ticks =
        g_ext4_lookup_parent_ref_ticks;
    ks->ext4_lookup_dir_find_ticks =
        g_ext4_lookup_dir_find_ticks;
    ks->ext4_lookup_found =
        g_ext4_lookup_found;
    ks->ext4_lookup_enoent =
        g_ext4_lookup_enoent;
    ks->ext4_lookup_errors =
        g_ext4_lookup_errors;

    ks->sys_open_calls =
        g_sys_open_calls;
    ks->sys_open_ticks =
        g_sys_open_ticks;
    ks->sys_fstat_calls =
        g_sys_fstat_calls;
    ks->sys_fstat_ticks =
        g_sys_fstat_ticks;
    ks->sys_lseek_calls =
        g_sys_lseek_calls;
    ks->sys_lseek_ticks =
        g_sys_lseek_ticks;
    ks->sys_pread64_calls =
        g_sys_pread64_calls;
    ks->sys_pread64_ticks =
        g_sys_pread64_ticks;
    ks->sys_openat_calls =
        g_sys_openat_calls;
    ks->sys_openat_ticks =
        g_sys_openat_ticks;
    ks->sys_fstatat_calls =
        g_sys_fstatat_calls;
    ks->sys_fstatat_ticks =
        g_sys_fstatat_ticks;
    ks->sys_faccessat_calls =
        g_sys_faccessat_calls;
    ks->sys_faccessat_ticks =
        g_sys_faccessat_ticks;
    ks->sys_read_calls =
        g_sys_read_calls;
    ks->sys_read_ticks =
        g_sys_read_ticks;
    ks->sys_readv_calls =
        g_sys_readv_calls;
    ks->sys_readv_ticks =
        g_sys_readv_ticks;
    ks->sys_getdents_calls =
        g_sys_getdents_calls;
    ks->sys_getdents_ticks =
        g_sys_getdents_ticks;
    ks->sys_readlinkat_calls =
        g_sys_readlinkat_calls;
    ks->sys_readlinkat_ticks =
        g_sys_readlinkat_ticks;
    ks->sys_mmap_calls =
        g_sys_mmap_calls;
    ks->sys_mmap_ticks =
        g_sys_mmap_ticks;
    ks->sys_munmap_calls =
        g_sys_munmap_calls;
    ks->sys_munmap_ticks =
        g_sys_munmap_ticks;
    ks->vm_munmap_pages_freed =
        g_vm_munmap_pages_freed;
    ks->vm_munmap_pte_walk_ticks =
        g_vm_munmap_pte_walk_ticks;
    ks->vm_munmap_page_release_ticks =
        g_vm_munmap_page_release_ticks;
    ks->vm_munmap_anon_pages =
        g_vm_munmap_anon_pages;
    ks->sys_mprotect_calls =
        g_sys_mprotect_calls;
    ks->sys_mprotect_ticks =
        g_sys_mprotect_ticks;
    ks->sys_brk_calls =
        g_sys_brk_calls;
    ks->sys_brk_ticks =
        g_sys_brk_ticks;
    ks->sys_clock_gettime_calls =
        g_sys_clock_gettime_calls;
    ks->sys_clock_gettime_ticks =
        g_sys_clock_gettime_ticks;
    ks->sys_clock_gettime_monotonic_calls =
        g_sys_clock_gettime_monotonic_calls;
    ks->sys_clock_gettime_monotonic_ticks =
        g_sys_clock_gettime_monotonic_ticks;
    ks->sys_clock_gettime_monotonic_coarse_calls =
        g_sys_clock_gettime_monotonic_coarse_calls;
    ks->sys_clock_gettime_monotonic_coarse_ticks =
        g_sys_clock_gettime_monotonic_coarse_ticks;
    ks->sys_clock_gettime_realtime_calls =
        g_sys_clock_gettime_realtime_calls;
    ks->sys_clock_gettime_realtime_ticks =
        g_sys_clock_gettime_realtime_ticks;
    ks->sys_clock_gettime_process_calls =
        g_sys_clock_gettime_process_calls;
    ks->sys_clock_gettime_process_ticks =
        g_sys_clock_gettime_process_ticks;
    ks->sys_clock_gettime_thread_calls =
        g_sys_clock_gettime_thread_calls;
    ks->sys_clock_gettime_thread_ticks =
        g_sys_clock_gettime_thread_ticks;
    ks->sys_clock_gettime_other_calls =
        g_sys_clock_gettime_other_calls;
    ks->sys_clock_gettime_other_ticks =
        g_sys_clock_gettime_other_ticks;
    ks->sys_gettimeofday_calls =
        g_sys_gettimeofday_calls;
    ks->sys_gettimeofday_ticks =
        g_sys_gettimeofday_ticks;
    ks->sys_getrandom_calls =
        g_sys_getrandom_calls;
    ks->sys_getrandom_ticks =
        g_sys_getrandom_ticks;
    ks->exec_calls =
        g_exec_calls;
    ks->exec_ticks =
        g_exec_ticks;
    ks->sys_openat_path_copy_calls =
        g_sys_openat_path_copy_calls;
    ks->sys_openat_path_copy_ticks =
        g_sys_openat_path_copy_ticks;
    ks->sys_openat_dirfd_calls =
        g_sys_openat_dirfd_calls;
    ks->sys_openat_dirfd_ticks =
        g_sys_openat_dirfd_ticks;
    ks->sys_openat_lookup_calls =
        g_sys_openat_lookup_calls;
    ks->sys_openat_lookup_ticks =
        g_sys_openat_lookup_ticks;
    ks->sys_openat_fileopen_calls =
        g_sys_openat_fileopen_calls;
    ks->sys_openat_fileopen_ticks =
        g_sys_openat_fileopen_ticks;
    ks->sys_openat_fdalloc_calls =
        g_sys_openat_fdalloc_calls;
    ks->sys_openat_fdalloc_ticks =
        g_sys_openat_fdalloc_ticks;
    ks->sys_poll_calls =
        g_sys_poll_calls;
    ks->sys_poll_ticks =
        g_sys_poll_ticks;
    ks->sys_ppoll_calls =
        g_sys_ppoll_calls;
    ks->sys_ppoll_ticks =
        g_sys_ppoll_ticks;
    ks->sys_poll_blocking_calls =
        g_sys_poll_blocking_calls;
    ks->sys_poll_blocking_ticks =
        g_sys_poll_blocking_ticks;
    ks->sys_ioctl_calls =
        g_sys_ioctl_calls;
    ks->sys_ioctl_ticks =
        g_sys_ioctl_ticks;
    ks->sys_ioctl_tty_tcgets_calls =
        g_sys_ioctl_tty_tcgets_calls;
    ks->sys_ioctl_tty_tcgets_ticks =
        g_sys_ioctl_tty_tcgets_ticks;
    ks->sys_ioctl_tty_tcsets_calls =
        g_sys_ioctl_tty_tcsets_calls;
    ks->sys_ioctl_tty_tcsets_ticks =
        g_sys_ioctl_tty_tcsets_ticks;
    ks->sys_ioctl_tty_winsz_calls =
        g_sys_ioctl_tty_winsz_calls;
    ks->sys_ioctl_tty_winsz_ticks =
        g_sys_ioctl_tty_winsz_ticks;
    ks->sys_ioctl_tty_pgrp_calls =
        g_sys_ioctl_tty_pgrp_calls;
    ks->sys_ioctl_tty_pgrp_ticks =
        g_sys_ioctl_tty_pgrp_ticks;
    ks->sys_ioctl_tty_ptmx_calls =
        g_sys_ioctl_tty_ptmx_calls;
    ks->sys_ioctl_tty_ptmx_ticks =
        g_sys_ioctl_tty_ptmx_ticks;
    ks->sys_ioctl_tty_ctty_calls =
        g_sys_ioctl_tty_ctty_calls;
    ks->sys_ioctl_tty_ctty_ticks =
        g_sys_ioctl_tty_ctty_ticks;
    ks->sys_futex_calls =
        g_sys_futex_calls;
    ks->sys_futex_ticks =
        g_sys_futex_ticks;
    ks->sys_futex_wait_calls =
        g_sys_futex_wait_calls;
    ks->sys_futex_wait_ticks =
        g_sys_futex_wait_ticks;
    ks->sys_futex_wake_calls =
        g_sys_futex_wake_calls;
    ks->sys_futex_wake_ticks =
        g_sys_futex_wake_ticks;
    ks->sys_poll_wait_unix_calls =
        g_sys_poll_wait_unix_calls;
    ks->sys_poll_wait_unix_ticks =
        g_sys_poll_wait_unix_ticks;
    ks->sys_poll_wait_eventfd_calls =
        g_sys_poll_wait_eventfd_calls;
    ks->sys_poll_wait_eventfd_ticks =
        g_sys_poll_wait_eventfd_ticks;
    ks->sys_poll_wait_pipe_calls =
        g_sys_poll_wait_pipe_calls;
    ks->sys_poll_wait_pipe_ticks =
        g_sys_poll_wait_pipe_ticks;
    ks->sys_poll_wait_other_calls =
        g_sys_poll_wait_other_calls;
    ks->sys_poll_wait_other_ticks =
        g_sys_poll_wait_other_ticks;
    ks->sys_poll_wait_notify_calls =
        g_sys_poll_wait_notify_calls;
    ks->sys_poll_wait_notify_ticks =
        g_sys_poll_wait_notify_ticks;
    ks->sys_poll_wait_rescan_calls =
        g_sys_poll_wait_rescan_calls;
    ks->sys_poll_wait_rescan_ticks =
        g_sys_poll_wait_rescan_ticks;
    ks->sys_poll_wait_ready_calls =
        g_sys_poll_wait_ready_calls;
    ks->sys_poll_wait_ready_ticks =
        g_sys_poll_wait_ready_ticks;
    ks->sys_poll_wait_timeout_calls =
        g_sys_poll_wait_timeout_calls;
    ks->sys_poll_wait_timeout_ticks =
        g_sys_poll_wait_timeout_ticks;
    ks->konsole_prepty_poll_total_calls =
        g_konsole_prepty_poll_total_calls;
    ks->konsole_prepty_poll_total_ticks =
        g_konsole_prepty_poll_total_ticks;
    ks->konsole_prepty_poll_wayland_calls =
        g_konsole_prepty_poll_wayland_calls;
    ks->konsole_prepty_poll_wayland_ticks =
        g_konsole_prepty_poll_wayland_ticks;
    ks->konsole_prepty_poll_qdbus_calls =
        g_konsole_prepty_poll_qdbus_calls;
    ks->konsole_prepty_poll_qdbus_ticks =
        g_konsole_prepty_poll_qdbus_ticks;
    ks->konsole_prepty_poll_unix_other_calls =
        g_konsole_prepty_poll_unix_other_calls;
    ks->konsole_prepty_poll_unix_other_ticks =
        g_konsole_prepty_poll_unix_other_ticks;
    ks->konsole_prepty_poll_eventfd_calls =
        g_konsole_prepty_poll_eventfd_calls;
    ks->konsole_prepty_poll_eventfd_ticks =
        g_konsole_prepty_poll_eventfd_ticks;
    ks->konsole_prepty_poll_pipe_calls =
        g_konsole_prepty_poll_pipe_calls;
    ks->konsole_prepty_poll_pipe_ticks =
        g_konsole_prepty_poll_pipe_ticks;
    ks->konsole_prepty_poll_other_calls =
        g_konsole_prepty_poll_other_calls;
    ks->konsole_prepty_poll_other_ticks =
        g_konsole_prepty_poll_other_ticks;
    ks->konsole_prepty_poll_ready_calls =
        g_konsole_prepty_poll_ready_calls;
    ks->konsole_prepty_poll_ready_ticks =
        g_konsole_prepty_poll_ready_ticks;
    ks->konsole_prepty_poll_timeout_calls =
        g_konsole_prepty_poll_timeout_calls;
    ks->konsole_prepty_poll_timeout_ticks =
        g_konsole_prepty_poll_timeout_ticks;
    ks->konsole_prepty_futex_wait_calls =
        g_konsole_prepty_futex_wait_calls;
    ks->konsole_prepty_futex_wait_ticks =
        g_konsole_prepty_futex_wait_ticks;
    ks->konsole_prepty_futex_woken_calls =
        g_konsole_prepty_futex_woken_calls;
    ks->konsole_prepty_futex_woken_ticks =
        g_konsole_prepty_futex_woken_ticks;
    ks->konsole_prepty_futex_timeout_calls =
        g_konsole_prepty_futex_timeout_calls;
    ks->konsole_prepty_futex_timeout_ticks =
        g_konsole_prepty_futex_timeout_ticks;
    ks->konsole_prepty_futex_signal_calls =
        g_konsole_prepty_futex_signal_calls;
    ks->konsole_prepty_futex_signal_ticks =
        g_konsole_prepty_futex_signal_ticks;
    ks->konsole_prepty_futex_other_calls =
        g_konsole_prepty_futex_other_calls;
    ks->konsole_prepty_futex_other_ticks =
        g_konsole_prepty_futex_other_ticks;
    ks->konsole_prepty_pty_seen =
        g_konsole_prepty_pty_seen;
    ks->konsole_prepty_poll_wayland_pipe_calls =
        g_konsole_prepty_poll_wayland_pipe_calls;
    ks->konsole_prepty_poll_wayland_pipe_ticks =
        g_konsole_prepty_poll_wayland_pipe_ticks;
    ks->konsole_prepty_poll_wayland_eventfd_calls =
        g_konsole_prepty_poll_wayland_eventfd_calls;
    ks->konsole_prepty_poll_wayland_eventfd_ticks =
        g_konsole_prepty_poll_wayland_eventfd_ticks;
    ks->konsole_prepty_poll_qdbus_pipe_calls =
        g_konsole_prepty_poll_qdbus_pipe_calls;
    ks->konsole_prepty_poll_qdbus_pipe_ticks =
        g_konsole_prepty_poll_qdbus_pipe_ticks;
    ks->konsole_prepty_poll_qdbus_eventfd_calls =
        g_konsole_prepty_poll_qdbus_eventfd_calls;
    ks->konsole_prepty_poll_qdbus_eventfd_ticks =
        g_konsole_prepty_poll_qdbus_eventfd_ticks;
    ks->konsole_prepty_poll_unix_other_pipe_calls =
        g_konsole_prepty_poll_unix_other_pipe_calls;
    ks->konsole_prepty_poll_unix_other_pipe_ticks =
        g_konsole_prepty_poll_unix_other_pipe_ticks;
    ks->konsole_prepty_poll_unix_other_eventfd_calls =
        g_konsole_prepty_poll_unix_other_eventfd_calls;
    ks->konsole_prepty_poll_unix_other_eventfd_ticks =
        g_konsole_prepty_poll_unix_other_eventfd_ticks;
    ks->konsole_prepty_poll_eventfd_pipe_calls =
        g_konsole_prepty_poll_eventfd_pipe_calls;
    ks->konsole_prepty_poll_eventfd_pipe_ticks =
        g_konsole_prepty_poll_eventfd_pipe_ticks;
    ks->konsole_prepty_poll_wayland_only_calls =
        g_konsole_prepty_poll_wayland_only_calls;
    ks->konsole_prepty_poll_wayland_only_ticks =
        g_konsole_prepty_poll_wayland_only_ticks;
    ks->konsole_prepty_poll_qdbus_only_calls =
        g_konsole_prepty_poll_qdbus_only_calls;
    ks->konsole_prepty_poll_qdbus_only_ticks =
        g_konsole_prepty_poll_qdbus_only_ticks;
    ks->konsole_prepty_poll_unix_other_only_calls =
        g_konsole_prepty_poll_unix_other_only_calls;
    ks->konsole_prepty_poll_unix_other_only_ticks =
        g_konsole_prepty_poll_unix_other_only_ticks;
    ks->konsole_prepty_poll_eventfd_only_calls =
        g_konsole_prepty_poll_eventfd_only_calls;
    ks->konsole_prepty_poll_eventfd_only_ticks =
        g_konsole_prepty_poll_eventfd_only_ticks;
    ks->konsole_prepty_poll_pipe_only_calls =
        g_konsole_prepty_poll_pipe_only_calls;
    ks->konsole_prepty_poll_pipe_only_ticks =
        g_konsole_prepty_poll_pipe_only_ticks;
    ks->konsole_prepty_poll_kqueue_wake_calls =
        g_konsole_prepty_poll_kqueue_wake_calls;
    ks->konsole_prepty_poll_kqueue_wake_ticks =
        g_konsole_prepty_poll_kqueue_wake_ticks;
    ks->konsole_prepty_poll_timed_rescan_calls =
        g_konsole_prepty_poll_timed_rescan_calls;
    ks->konsole_prepty_poll_timed_rescan_ticks =
        g_konsole_prepty_poll_timed_rescan_ticks;
    ks->konsole_prepty_poll_rescan_ready_calls =
        g_konsole_prepty_poll_rescan_ready_calls;
    ks->konsole_prepty_poll_rescan_ready_ticks =
        g_konsole_prepty_poll_rescan_ready_ticks;
    ks->konsole_prepty_poll_event_ready_calls =
        g_konsole_prepty_poll_event_ready_calls;
    ks->konsole_prepty_poll_event_ready_ticks =
        g_konsole_prepty_poll_event_ready_ticks;
    ks->konsole_prepty_poll_ready_wayland_calls =
        g_konsole_prepty_poll_ready_wayland_calls;
    ks->konsole_prepty_poll_ready_wayland_ticks =
        g_konsole_prepty_poll_ready_wayland_ticks;
    ks->konsole_prepty_poll_ready_qdbus_calls =
        g_konsole_prepty_poll_ready_qdbus_calls;
    ks->konsole_prepty_poll_ready_qdbus_ticks =
        g_konsole_prepty_poll_ready_qdbus_ticks;
    ks->konsole_prepty_poll_ready_unix_other_calls =
        g_konsole_prepty_poll_ready_unix_other_calls;
    ks->konsole_prepty_poll_ready_unix_other_ticks =
        g_konsole_prepty_poll_ready_unix_other_ticks;
    ks->konsole_prepty_poll_ready_eventfd_calls =
        g_konsole_prepty_poll_ready_eventfd_calls;
    ks->konsole_prepty_poll_ready_eventfd_ticks =
        g_konsole_prepty_poll_ready_eventfd_ticks;
    ks->konsole_prepty_poll_ready_pipe_calls =
        g_konsole_prepty_poll_ready_pipe_calls;
    ks->konsole_prepty_poll_ready_pipe_ticks =
        g_konsole_prepty_poll_ready_pipe_ticks;
    ks->konsole_prepty_poll_rescan_ready_wayland_calls =
        g_konsole_prepty_poll_rescan_ready_wayland_calls;
    ks->konsole_prepty_poll_rescan_ready_wayland_ticks =
        g_konsole_prepty_poll_rescan_ready_wayland_ticks;
    ks->konsole_prepty_poll_rescan_ready_qdbus_calls =
        g_konsole_prepty_poll_rescan_ready_qdbus_calls;
    ks->konsole_prepty_poll_rescan_ready_qdbus_ticks =
        g_konsole_prepty_poll_rescan_ready_qdbus_ticks;
    ks->konsole_prepty_poll_rescan_ready_unix_other_calls =
        g_konsole_prepty_poll_rescan_ready_unix_other_calls;
    ks->konsole_prepty_poll_rescan_ready_unix_other_ticks =
        g_konsole_prepty_poll_rescan_ready_unix_other_ticks;
    ks->konsole_prepty_poll_rescan_ready_eventfd_calls =
        g_konsole_prepty_poll_rescan_ready_eventfd_calls;
    ks->konsole_prepty_poll_rescan_ready_eventfd_ticks =
        g_konsole_prepty_poll_rescan_ready_eventfd_ticks;
    ks->konsole_prepty_poll_rescan_ready_pipe_calls =
        g_konsole_prepty_poll_rescan_ready_pipe_calls;
    ks->konsole_prepty_poll_rescan_ready_pipe_ticks =
        g_konsole_prepty_poll_rescan_ready_pipe_ticks;
}

void kstats_profile_set(int enabled) {
    __atomic_store_n(&g_kstats_profile_enabled, enabled != 0,
                     __ATOMIC_RELAXED);
}

static int kstats_konsole_path_match(const char *path)
{
    return path != NULL && strstr(path, "/konsole") != NULL;
}

int kstats_konsole_prepty_current(void)
{
    struct thread_group *tg;

    if (!kstats_profile_enabled() || current == NULL)
        return 0;
    tg = current->thread_group;
    if (tg == NULL)
        return 0;
    return __atomic_load_n(&tg->konsole_prepty_active,
                           __ATOMIC_RELAXED) != 0 &&
           __atomic_load_n(&tg->konsole_prepty_pty_seen,
                           __ATOMIC_RELAXED) == 0;
}

void kstats_konsole_prepty_exec(const char *path)
{
    struct thread_group *tg;
    int active;

    if (current == NULL || current->thread_group == NULL)
        return;
    tg = current->thread_group;
    active = kstats_konsole_path_match(path);
    __atomic_store_n(&tg->konsole_prepty_active, active, __ATOMIC_RELAXED);
    __atomic_store_n(&tg->konsole_prepty_pty_seen, 0, __ATOMIC_RELAXED);
}

void kstats_konsole_prepty_mark_pty(void)
{
    struct thread_group *tg;

    if (current == NULL || current->thread_group == NULL)
        return;
    tg = current->thread_group;
    if (__atomic_load_n(&tg->konsole_prepty_active, __ATOMIC_RELAXED) == 0)
        return;
    if (__atomic_exchange_n(&tg->konsole_prepty_pty_seen, 1,
                            __ATOMIC_RELAXED) == 0) {
        __atomic_add_fetch(&g_konsole_prepty_pty_seen, 1,
                           __ATOMIC_RELAXED);
    }
    __atomic_store_n(&tg->konsole_prepty_active, 0, __ATOMIC_RELAXED);
}

void kstats_konsole_prepty_futex_account(int ret, uint64 wait_ticks)
{
    if (!kstats_konsole_prepty_current() || wait_ticks == 0)
        return;

    __atomic_add_fetch(&g_konsole_prepty_futex_wait_calls, 1,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_konsole_prepty_futex_wait_ticks, wait_ticks,
                       __ATOMIC_RELAXED);
    if (ret == 0) {
        __atomic_add_fetch(&g_konsole_prepty_futex_woken_calls, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_konsole_prepty_futex_woken_ticks, wait_ticks,
                           __ATOMIC_RELAXED);
    } else if (ret == -ETIMEDOUT) {
        __atomic_add_fetch(&g_konsole_prepty_futex_timeout_calls, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_konsole_prepty_futex_timeout_ticks, wait_ticks,
                           __ATOMIC_RELAXED);
    } else if (ret == -EINTR) {
        __atomic_add_fetch(&g_konsole_prepty_futex_signal_calls, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_konsole_prepty_futex_signal_ticks, wait_ticks,
                           __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&g_konsole_prepty_futex_other_calls, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_konsole_prepty_futex_other_ticks, wait_ticks,
                           __ATOMIC_RELAXED);
    }
}

static uint64 kstats_copyout(uint64 uaddr, uint64 usize) {
    struct kstats ks;
    uint64 n;
    uint64 off;
    char zeros[64];

    if (uaddr == 0 || usize == 0)
        return -EINVAL;

    kstats_collect(&ks);

    n = usize < sizeof(ks) ? usize : sizeof(ks);
    if (vm_copyout(current->vm, uaddr, (char *)&ks, n) < 0)
        return -EFAULT;

    if (usize > sizeof(ks)) {
        memset(zeros, 0, sizeof(zeros));
        for (off = sizeof(ks); off < usize; off += sizeof(zeros)) {
            n = usize - off;
            if (n > sizeof(zeros))
                n = sizeof(zeros);
            if (vm_copyout(current->vm, uaddr + off, zeros, n) < 0)
                return -EFAULT;
        }
    }

    return 0;
}

/*
 * sys_kstats(uaddr)
 *
 * Legacy one-argument ABI. Copy only the stable v1 prefix so old binaries with
 * a smaller struct are not overrun when new diagnostic counters are appended.
 */
uint64 sys_kstats(void) {
    uint64 uaddr;
    argaddr(0, &uaddr);
    return kstats_copyout(uaddr, KSTATS_ABI_V1_SIZE);
}

/*
 * sys_kstats2(uaddr, size)
 *
 * Size-aware ABI for append-only kstats growth.
 */
uint64 sys_kstats2(void) {
    uint64 uaddr;
    uint64 usize;
    argaddr(0, &uaddr);
    argaddr(1, &usize);
    return kstats_copyout(uaddr, usize);
}

uint64 sys_kstatsctl(void) {
    int enabled;
    argint(0, &enabled);
    kstats_profile_set(enabled != 0);
    return 0;
}
