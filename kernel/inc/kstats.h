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

/* Maximum CPUs reported — matches NCPU from param.h */
#define KSTATS_MAX_CPUS NCPU

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
};

/* ------------------------------------------------------------------ */
/*  Global atomic counters (defined in bio.c / e1000.c)               */
/* ------------------------------------------------------------------ */

extern _Atomic uint64 g_bio_reads;
extern _Atomic uint64 g_bio_writes;
extern _Atomic uint64 g_bio_read_bytes;
extern _Atomic uint64 g_bio_write_bytes;

extern _Atomic uint64 g_net_tx_packets;
extern _Atomic uint64 g_net_tx_bytes;
extern _Atomic uint64 g_net_rx_packets;
extern _Atomic uint64 g_net_rx_bytes;

/* ------------------------------------------------------------------ */
/*  API                                                               */
/* ------------------------------------------------------------------ */

/** Fill a kstats struct with a point-in-time snapshot. */
void kstats_collect(struct kstats *ks);

#endif /* __KERNEL_KSTATS_H */
