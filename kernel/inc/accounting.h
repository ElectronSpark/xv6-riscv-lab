/**
 * @file accounting.h
 * @brief Per-process resource accounting counters
 *
 * Defines struct proc_acct — a set of cumulative counters embedded in
 * each thread_group that track resource consumption across FS, BIO,
 * network, memory-management, and scheduler subsystems.
 *
 * All counters use GCC atomic builtins so they can be updated without
 * holding any lock.  The ACCT_ADD / ACCT_INC macros expand to no-ops
 * when ACCT_ENABLED is 0, giving zero overhead in production builds.
 *
 * Counters are exposed to user-space via /proc/<pid>/resources.
 */

#ifndef __KERNEL_ACCOUNTING_H
#define __KERNEL_ACCOUNTING_H

#include "types.h"

/* Per-process accounting is always enabled */
#define ACCT_ENABLED 1

/**
 * @brief Per-process (thread_group) resource accounting counters.
 *
 * All fields are updated atomically with relaxed ordering — they are
 * statistical counters, not synchronisation variables.
 */
struct proc_acct {
    /* ── File System ─────────────────────────────────────────────── */
    _Atomic uint64 fs_opens;          /* successful open/openat calls    */
    _Atomic uint64 fs_closes;         /* close calls                     */
    _Atomic uint64 fs_bytes_read;     /* bytes returned by read/readv    */
    _Atomic uint64 fs_bytes_written;  /* bytes returned by write/writev  */
    _Atomic uint64 fs_creates;        /* mkdir + mknod + creat           */
    _Atomic uint64 fs_deletes;        /* unlink + rmdir                  */
    _Atomic uint64 fs_renames;        /* rename + renameat               */
    _Atomic uint64 fs_links;          /* link + linkat + symlink + ...   */
    _Atomic uint64 fs_chdirs;         /* chdir + chroot                  */
    _Atomic uint64 fs_mounts;         /* mount + umount                  */

    /* ── Block I/O ───────────────────────────────────────────────── */
    _Atomic uint64 bio_reads;         /* bread call count                */
    _Atomic uint64 bio_writes;        /* bwrite/bwrite_async call count  */

    /* ── Network ─────────────────────────────────────────────────── */
    _Atomic uint64 net_sockets;       /* socket() calls (success)        */
    _Atomic uint64 net_connects;      /* connect() calls (success)       */
    _Atomic uint64 net_accepts;       /* accept/accept4() (success)      */
    _Atomic uint64 net_bytes_sent;    /* bytes via sendto/sendmsg        */
    _Atomic uint64 net_bytes_recv;    /* bytes via recvfrom/recvmsg      */

    /* ── Memory Management ───────────────────────────────────────── */
    _Atomic uint64 mm_mmap_count;     /* mmap() calls (success)          */
    _Atomic uint64 mm_munmap_count;   /* munmap() calls                  */
    _Atomic int64  mm_brk_delta;      /* cumulative brk delta (bytes)    */
    _Atomic uint64 mm_rss_pages;      /* live resident pages             */
    _Atomic uint64 mm_peak_vm;        /* peak virtual memory (bytes)     */

    /* ── Scheduler / Process ─────────────────────────────────────── */
    _Atomic uint64 sched_forks;       /* fork/clone (new tg) count       */
    _Atomic uint64 sched_execs;       /* exec() count                    */
    _Atomic uint64 sched_exits;       /* exit/exit_group count           */
};

/* ------------------------------------------------------------------ */
/*  Counter manipulation macros                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Atomically add @a delta to counter @a field in the current
 *        process's proc_acct struct.
 *
 * @param tg     pointer to struct thread_group
 * @param field  field name in struct proc_acct  (e.g. fs_opens)
 * @param delta  value to add (usually 1 or byte count)
 */
#define ACCT_ADD(tg, field, delta) \
    __atomic_fetch_add(&(tg)->acct.field, (delta), __ATOMIC_RELAXED)

/**
 * @brief Atomically store max(current, @a val) into @a field.
 */
#define ACCT_MAX(tg, field, val) do {                              \
    uint64 __acct_v = (val);                                       \
    uint64 __acct_old = __atomic_load_n(&(tg)->acct.field,         \
                                        __ATOMIC_RELAXED);         \
    while (__acct_v > __acct_old) {                                \
        if (__atomic_compare_exchange_n(&(tg)->acct.field,         \
                    &__acct_old, __acct_v, 1,                      \
                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))           \
            break;                                                 \
    }                                                              \
} while (0)

/** Convenience: increment field by 1 */
#define ACCT_INC(tg, field) ACCT_ADD(tg, field, 1)

/* ------------------------------------------------------------------ */
/*  API                                                               */
/* ------------------------------------------------------------------ */

struct thread_group;

/** Zero-initialise all accounting counters in a thread_group. */
void acct_init(struct thread_group *tg);

/**
 * @brief Format accounting counters + rlimits as key-value text.
 *
 * Writes a human-readable representation to @a buf, at most @a sz
 * bytes (including NUL).  Returns the number of characters written
 * (excluding NUL).
 */
int acct_format(struct thread_group *tg, char *buf, int sz);

#endif /* __KERNEL_ACCOUNTING_H */
