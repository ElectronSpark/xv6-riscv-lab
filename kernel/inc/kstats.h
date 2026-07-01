/**
 * @file kstats.h
 * @brief System-wide kernel statistics (CPU / Disk / Network)
 *
 * Provides:
 *  - struct kstats — snapshot of system-wide counters
 *  - Global atomic counters bumped by bio and net drivers
 *  - sys_kstats() syscall to copy a snapshot to user-space
 *
 * Shared between kernel and user-space (via copy-out).
 */

#ifndef __KERNEL_KSTATS_H
#define __KERNEL_KSTATS_H

#include "types.h"
#include "param.h"

/* User ABI capacity for per-CPU statistics; runtime count is in kstats.ncpus. */
#define KSTATS_MAX_CPUS 64

/** Per-CPU scheduler snapshot (read-only to user-space). */
struct cpu_stat {
    uint32 nr_running;   /**< runnable entities on this CPU       */
    uint32 idle;         /**< 1 if CPU is currently idle           */
    uint64 load_avg;     /**< EWMA smoothed load (fixed-point)    */
    uint64 cpu_util;     /**< sum of per-entity util_avg (PELT)   */
    uint64 cpu_load;     /**< sum of per-entity weighted util     */
    uint64 busy_ticks;   /**< cumulative non-idle timer ticks     */
    uint64 total_ticks;  /**< cumulative total timer ticks        */
    uint64 util_1s;      /**< CPU utilization over last 1s (FSHIFT=11 fp) */
};

/** System-wide statistics snapshot. */
struct kstats {
    /* ── General ────────────────────────────────────────────────── */
    uint64 uptime_ms;         /**< milliseconds since boot            */
    int    ncpus;             /**< number of active CPUs              */
    uint64 timebase_freq;     /**< timer frequency (r_time() ticks/s) */
    uint64 timestamp;         /**< raw r_time() snapshot (same clock as
                                   per-task sum_exec_runtime)         */

    /* ── Load averages (fixed-point, FSHIFT=11) ─────────────────── */
    uint64 load_avg_1s;       /**< 1-second EWMA load average         */
    uint64 load_avg_5s;       /**< 5-second EWMA load average         */
    uint64 load_avg_16s;      /**< 16-second EWMA load average        */

    /* ── Per-CPU ────────────────────────────────────────────────── */
    struct cpu_stat cpu[KSTATS_MAX_CPUS];

    /* ── Block I/O (global) ─────────────────────────────────────── */
    uint64 bio_reads;         /**< bread() call count                 */
    uint64 bio_writes;        /**< bwrite/bwrite_async call count     */
    uint64 bio_read_bytes;    /**< bytes read from disk               */
    uint64 bio_write_bytes;   /**< bytes written to disk              */

    /* ── Network (global) ───────────────────────────────────────── */
    uint64 net_tx_packets;    /**< packets transmitted                */
    uint64 net_tx_bytes;      /**< bytes transmitted                  */
    uint64 net_rx_packets;    /**< packets received                   */
    uint64 net_rx_bytes;      /**< bytes received                     */

    /* ── VFS lookup hot path ────────────────────────────────────── */
    uint64 vfs_lookup_calls;
    uint64 vfs_lookup_dcache_hits;
    uint64 vfs_lookup_negative_hits;
    uint64 vfs_lookup_cache_misses;
    uint64 vfs_lookup_driver_calls;
    uint64 vfs_lookup_driver_ticks;
    uint64 vfs_dentry_inode_calls;
    uint64 vfs_dentry_inode_ticks;
    uint64 vfs_dentry_inode_retries;
    uint64 vfs_dentry_inode_self_hits;
    uint64 vfs_dentry_inode_rlock_calls;
    uint64 vfs_dentry_inode_rlock_ticks;
    uint64 vfs_dentry_inode_upgrade_calls;
    uint64 vfs_dentry_inode_upgrade_ticks;
    uint64 vfs_inode_cache_calls;
    uint64 vfs_inode_cache_hits;
    uint64 vfs_inode_cache_misses;
    uint64 vfs_inode_cache_eagain;
    uint64 vfs_inode_cache_miss_hash;
    uint64 vfs_inode_cache_miss_revive_without_wlock;
    uint64 vfs_inode_cache_miss_dying;
    uint64 vfs_inode_cache_miss_invalid_destroying;
    uint64 vfs_inode_cache_read_revive_attempts;
    uint64 vfs_inode_cache_read_revive_success;
    uint64 vfs_inode_cache_read_revive_lock_fail;
    uint64 vfs_inode_cache_read_revive_stale;
    uint64 vfs_inode_cache_ticks;
    uint64 vfs_inode_load_calls;
    uint64 vfs_inode_load_success;
    uint64 vfs_inode_load_ticks;

    /* ── VM user-copy / fault path ──────────────────────────────── */
    uint64 vm_copyin_calls;
    uint64 vm_copyout_calls;
    uint64 vm_copyin_bytes;
    uint64 vm_copyout_bytes;
    uint64 vm_copyout_fast_hits;
    uint64 vm_copyout_fast_bytes;
    uint64 vm_copyin_fast_hits;
    uint64 vm_copyin_fast_bytes;
    uint64 vm_copyout_present_skip_hits;
    uint64 vm_copyout_present_skip_bytes;
    uint64 vm_copyin_present_skip_hits;
    uint64 vm_copyin_present_skip_bytes;
    uint64 vm_vma_validate_calls;
    uint64 vm_vma_validate_ticks;
    uint64 vm_file_faults;
    uint64 vm_validate_batch_ticks;
    uint64 vm_validate_fallback_ticks;
    uint64 vm_validate_hugepage_ticks;
    uint64 vm_validate_pte_check_ticks;
    uint64 vm_copyin_ticks;
    uint64 vm_copyout_ticks;

    /* ── ext4 import/read hot path ──────────────────────────────── */
    uint64 ext4_pcache_read_page_calls;
    uint64 ext4_pcache_pages_filled;
    uint64 ext4_pcache_readahead_pages;
    uint64 ext4_pcache_read_page_ticks;
    uint64 ext4_fault_calls;
    uint64 ext4_fault_zero_copy;
    uint64 ext4_fault_partial_copy;
    uint64 ext4_fault_ticks;
    uint64 ext4_lookup_calls;
    uint64 ext4_lookup_lock_wait_ticks;
    uint64 ext4_lookup_lock_hold_ticks;
    uint64 ext4_lookup_parent_ref_ticks;
    uint64 ext4_lookup_dir_find_ticks;
    uint64 ext4_lookup_found;
    uint64 ext4_lookup_enoent;
    uint64 ext4_lookup_errors;

    /* ── Startup syscall / exec path ────────────────────────────── */
    uint64 sys_open_calls;
    uint64 sys_open_ticks;
    uint64 sys_fstat_calls;
    uint64 sys_fstat_ticks;
    uint64 sys_lseek_calls;
    uint64 sys_lseek_ticks;
    uint64 sys_pread64_calls;
    uint64 sys_pread64_ticks;
    uint64 sys_openat_calls;
    uint64 sys_openat_ticks;
    uint64 sys_fstatat_calls;
    uint64 sys_fstatat_ticks;
    uint64 sys_faccessat_calls;
    uint64 sys_faccessat_ticks;
    uint64 sys_read_calls;
    uint64 sys_read_ticks;
    uint64 sys_readv_calls;
    uint64 sys_readv_ticks;
    uint64 sys_getdents_calls;
    uint64 sys_getdents_ticks;
    uint64 sys_readlinkat_calls;
    uint64 sys_readlinkat_ticks;
    uint64 sys_mmap_calls;
    uint64 sys_mmap_ticks;
    uint64 sys_munmap_calls;
    uint64 sys_munmap_ticks;
    uint64 vm_munmap_pages_freed;
    uint64 vm_munmap_pte_walk_ticks;
    uint64 vm_munmap_page_release_ticks;
    uint64 vm_munmap_anon_pages;
    uint64 sys_mprotect_calls;
    uint64 sys_mprotect_ticks;
    uint64 sys_brk_calls;
    uint64 sys_brk_ticks;
    uint64 sys_clock_gettime_calls;
    uint64 sys_clock_gettime_ticks;
    uint64 sys_clock_gettime_monotonic_calls;
    uint64 sys_clock_gettime_monotonic_ticks;
    uint64 sys_clock_gettime_monotonic_coarse_calls;
    uint64 sys_clock_gettime_monotonic_coarse_ticks;
    uint64 sys_clock_gettime_realtime_calls;
    uint64 sys_clock_gettime_realtime_ticks;
    uint64 sys_clock_gettime_process_calls;
    uint64 sys_clock_gettime_process_ticks;
    uint64 sys_clock_gettime_thread_calls;
    uint64 sys_clock_gettime_thread_ticks;
    uint64 sys_clock_gettime_other_calls;
    uint64 sys_clock_gettime_other_ticks;
    uint64 sys_gettimeofday_calls;
    uint64 sys_gettimeofday_ticks;
    uint64 sys_getrandom_calls;
    uint64 sys_getrandom_ticks;
    uint64 exec_calls;
    uint64 exec_ticks;
    uint64 sys_openat_path_copy_calls;
    uint64 sys_openat_path_copy_ticks;
    uint64 sys_openat_dirfd_calls;
    uint64 sys_openat_dirfd_ticks;
    uint64 sys_openat_lookup_calls;
    uint64 sys_openat_lookup_ticks;
    uint64 sys_openat_fileopen_calls;
    uint64 sys_openat_fileopen_ticks;
    uint64 sys_openat_fdalloc_calls;
    uint64 sys_openat_fdalloc_ticks;
};

/* ------------------------------------------------------------------ */
/*  Global atomic counters (defined in bio.c / e1000.c)               */
/* ------------------------------------------------------------------ */

extern uint64 g_bio_reads;
extern uint64 g_bio_writes;
extern uint64 g_bio_read_bytes;
extern uint64 g_bio_write_bytes;

extern uint64 g_net_tx_packets;
extern uint64 g_net_tx_bytes;
extern uint64 g_net_rx_packets;
extern uint64 g_net_rx_bytes;

extern uint64 g_vfs_lookup_calls;
extern uint64 g_vfs_lookup_dcache_hits;
extern uint64 g_vfs_lookup_negative_hits;
extern uint64 g_vfs_lookup_cache_misses;
extern uint64 g_vfs_lookup_driver_calls;
extern uint64 g_vfs_lookup_driver_ticks;
extern uint64 g_vfs_dentry_inode_calls;
extern uint64 g_vfs_dentry_inode_ticks;
extern uint64 g_vfs_dentry_inode_retries;
extern uint64 g_vfs_dentry_inode_self_hits;
extern uint64 g_vfs_dentry_inode_rlock_calls;
extern uint64 g_vfs_dentry_inode_rlock_ticks;
extern uint64 g_vfs_dentry_inode_upgrade_calls;
extern uint64 g_vfs_dentry_inode_upgrade_ticks;
extern uint64 g_vfs_inode_cache_calls;
extern uint64 g_vfs_inode_cache_hits;
extern uint64 g_vfs_inode_cache_misses;
extern uint64 g_vfs_inode_cache_eagain;
extern uint64 g_vfs_inode_cache_miss_hash;
extern uint64 g_vfs_inode_cache_miss_revive_without_wlock;
extern uint64 g_vfs_inode_cache_miss_dying;
extern uint64 g_vfs_inode_cache_miss_invalid_destroying;
extern uint64 g_vfs_inode_cache_read_revive_attempts;
extern uint64 g_vfs_inode_cache_read_revive_success;
extern uint64 g_vfs_inode_cache_read_revive_lock_fail;
extern uint64 g_vfs_inode_cache_read_revive_stale;
extern uint64 g_vfs_inode_cache_ticks;
extern uint64 g_vfs_inode_load_calls;
extern uint64 g_vfs_inode_load_success;
extern uint64 g_vfs_inode_load_ticks;

extern uint64 g_vm_copyin_calls;
extern uint64 g_vm_copyout_calls;
extern uint64 g_vm_copyin_bytes;
extern uint64 g_vm_copyout_bytes;
extern uint64 g_vm_copyout_fast_hits;
extern uint64 g_vm_copyout_fast_bytes;
extern uint64 g_vm_copyin_fast_hits;
extern uint64 g_vm_copyin_fast_bytes;
extern uint64 g_vm_copyout_present_skip_hits;
extern uint64 g_vm_copyout_present_skip_bytes;
extern uint64 g_vm_copyin_present_skip_hits;
extern uint64 g_vm_copyin_present_skip_bytes;
extern uint64 g_vm_vma_validate_calls;
extern uint64 g_vm_vma_validate_ticks;
extern uint64 g_vm_file_faults;
extern uint64 g_vm_validate_batch_ticks;
extern uint64 g_vm_validate_fallback_ticks;
extern uint64 g_vm_validate_hugepage_ticks;
extern uint64 g_vm_validate_pte_check_ticks;
extern uint64 g_vm_copyin_ticks;
extern uint64 g_vm_copyout_ticks;

extern uint64 g_ext4_pcache_read_page_calls;
extern uint64 g_ext4_pcache_pages_filled;
extern uint64 g_ext4_pcache_readahead_pages;
extern uint64 g_ext4_pcache_read_page_ticks;
extern uint64 g_ext4_fault_calls;
extern uint64 g_ext4_fault_zero_copy;
extern uint64 g_ext4_fault_partial_copy;
extern uint64 g_ext4_fault_ticks;
extern uint64 g_ext4_lookup_calls;
extern uint64 g_ext4_lookup_lock_wait_ticks;
extern uint64 g_ext4_lookup_lock_hold_ticks;
extern uint64 g_ext4_lookup_parent_ref_ticks;
extern uint64 g_ext4_lookup_dir_find_ticks;
extern uint64 g_ext4_lookup_found;
extern uint64 g_ext4_lookup_enoent;
extern uint64 g_ext4_lookup_errors;

extern uint64 g_sys_open_calls;
extern uint64 g_sys_open_ticks;
extern uint64 g_sys_fstat_calls;
extern uint64 g_sys_fstat_ticks;
extern uint64 g_sys_lseek_calls;
extern uint64 g_sys_lseek_ticks;
extern uint64 g_sys_pread64_calls;
extern uint64 g_sys_pread64_ticks;
extern uint64 g_sys_openat_calls;
extern uint64 g_sys_openat_ticks;
extern uint64 g_sys_fstatat_calls;
extern uint64 g_sys_fstatat_ticks;
extern uint64 g_sys_faccessat_calls;
extern uint64 g_sys_faccessat_ticks;
extern uint64 g_sys_read_calls;
extern uint64 g_sys_read_ticks;
extern uint64 g_sys_readv_calls;
extern uint64 g_sys_readv_ticks;
extern uint64 g_sys_getdents_calls;
extern uint64 g_sys_getdents_ticks;
extern uint64 g_sys_readlinkat_calls;
extern uint64 g_sys_readlinkat_ticks;
extern uint64 g_sys_mmap_calls;
extern uint64 g_sys_mmap_ticks;
extern uint64 g_sys_munmap_calls;
extern uint64 g_sys_munmap_ticks;
extern uint64 g_vm_munmap_pages_freed;
extern uint64 g_vm_munmap_pte_walk_ticks;
extern uint64 g_vm_munmap_page_release_ticks;
extern uint64 g_vm_munmap_anon_pages;
extern uint64 g_sys_mprotect_calls;
extern uint64 g_sys_mprotect_ticks;
extern uint64 g_sys_brk_calls;
extern uint64 g_sys_brk_ticks;
extern uint64 g_sys_clock_gettime_calls;
extern uint64 g_sys_clock_gettime_ticks;
extern uint64 g_sys_clock_gettime_monotonic_calls;
extern uint64 g_sys_clock_gettime_monotonic_ticks;
extern uint64 g_sys_clock_gettime_monotonic_coarse_calls;
extern uint64 g_sys_clock_gettime_monotonic_coarse_ticks;
extern uint64 g_sys_clock_gettime_realtime_calls;
extern uint64 g_sys_clock_gettime_realtime_ticks;
extern uint64 g_sys_clock_gettime_process_calls;
extern uint64 g_sys_clock_gettime_process_ticks;
extern uint64 g_sys_clock_gettime_thread_calls;
extern uint64 g_sys_clock_gettime_thread_ticks;
extern uint64 g_sys_clock_gettime_other_calls;
extern uint64 g_sys_clock_gettime_other_ticks;
extern uint64 g_sys_gettimeofday_calls;
extern uint64 g_sys_gettimeofday_ticks;
extern uint64 g_sys_getrandom_calls;
extern uint64 g_sys_getrandom_ticks;
extern uint64 g_exec_calls;
extern uint64 g_exec_ticks;
extern uint64 g_sys_openat_path_copy_calls;
extern uint64 g_sys_openat_path_copy_ticks;
extern uint64 g_sys_openat_dirfd_calls;
extern uint64 g_sys_openat_dirfd_ticks;
extern uint64 g_sys_openat_lookup_calls;
extern uint64 g_sys_openat_lookup_ticks;
extern uint64 g_sys_openat_fileopen_calls;
extern uint64 g_sys_openat_fileopen_ticks;
extern uint64 g_sys_openat_fdalloc_calls;
extern uint64 g_sys_openat_fdalloc_ticks;
extern int g_kstats_profile_enabled;

static inline int kstats_profile_enabled(void) {
    return __atomic_load_n(&g_kstats_profile_enabled, __ATOMIC_RELAXED);
}

#define KSTATS_PROFILE_INC(counter)                                         \
    do {                                                                    \
        if (kstats_profile_enabled())                                       \
            __atomic_add_fetch(&(counter), 1, __ATOMIC_RELAXED);           \
    } while (0)

#define KSTATS_PROFILE_ADD(counter, value)                                  \
    do {                                                                    \
        if (kstats_profile_enabled())                                       \
            __atomic_add_fetch(&(counter), (value), __ATOMIC_RELAXED);     \
    } while (0)

/* ------------------------------------------------------------------ */
/*  API                                                               */
/* ------------------------------------------------------------------ */

/** Fill a kstats struct with a point-in-time snapshot. */
void kstats_collect(struct kstats *ks);
void kstats_profile_set(int enabled);

#endif /* __KERNEL_KSTATS_H */
