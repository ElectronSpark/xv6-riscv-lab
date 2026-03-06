/**
 * @file iosched_types.h
 * @brief Block I/O scheduler type definitions.
 *
 * The IO scheduler sits between the buffer cache (bio.c) and the block
 * device drivers.  It accepts struct bio submissions, inserts them into
 * per-device sorted queues, and dispatches them in an order that
 * minimises seek time (elevator / C-SCAN) or provides simple FIFO
 * ordering (NOOP).
 *
 * Key design points:
 *   - Per-device scheduler state (struct iosched) embedded in blkdev_t.
 *   - Separate read and write queues, each sorted ascending by sector.
 *   - Read-priority with write starvation prevention.
 *   - Uses bio->list_entry for queue linkage (no extra allocation).
 *   - Back-merge: if a new bio is contiguous after an already-queued
 *     bio (same direction, adjacent sectors), the scheduler records
 *     the merge so callers can create a single larger I/O.
 */

#ifndef __KERNEL_IOSCHED_TYPES_H
#define __KERNEL_IOSCHED_TYPES_H

#include <compiler.h>
#include <types.h>
#include <list_type.h>
#include <lock/spinlock.h>

/* Forward declarations */
typedef struct blkdev blkdev_t;
struct bio;

/* ── Scheduling policies ─────────────────────────────────────────── */

/**
 * enum iosched_policy - Available IO scheduling algorithms.
 * @IOSCHED_NOOP:  First-in first-out.  No reordering.
 * @IOSCHED_CSCAN: Circular-SCAN elevator.  Dispatches in ascending
 *                 sector order, then wraps to the lowest pending
 *                 sector (one-way sweep).  Separate read & write
 *                 queues with read priority.
 */
enum iosched_policy {
    IOSCHED_NOOP  = 0,
    IOSCHED_CSCAN = 1,
};

/* ── Tunables ─────────────────────────────────────────────────────── */

/** Max bios returned by a single iosched_dispatch_batch() call. */
#define IOSCHED_BATCH_MAX          64

/**
 * After dispatching this many consecutive reads, the scheduler forces
 * a batch of writes (if any are pending) to prevent write starvation.
 */
#define IOSCHED_WRITE_STARVE_LIMIT 8

/* ── Statistics ───────────────────────────────────────────────────── */

struct iosched_stats {
    _Atomic uint64 enqueued;     /**< Total bios added to the scheduler */
    _Atomic uint64 dispatched;   /**< Total bios sent to the driver    */
    _Atomic uint64 requeued;     /**< Bios re-inserted after error     */
};

/* ── IO Scheduler state ───────────────────────────────────────────── */

/**
 * struct iosched - Per-device IO scheduler.
 *
 * Embed this in blkdev_t.  All fields protected by @lock unless
 * noted otherwise (stats are atomic).
 */
struct iosched {
    spinlock_t          lock;

    blkdev_t           *bdev;           /**< Back-pointer to owning device */
    enum iosched_policy policy;

    /* ── Per-direction sorted queues ──
     * Bios are linked via bio->list_entry and sorted ascending by
     * bio->blkno.  The list head is a sentinel; the first real entry
     * after the head has the smallest sector number. */
    list_node_t         read_queue;
    list_node_t         write_queue;
    int                 nr_reads;
    int                 nr_writes;

    /* ── C-SCAN elevator state ── */
    uint64              arm_position;   /**< Sector of last dispatch */
    int                 read_batch;     /**< Reads since last write drain */

    /* ── Statistics (atomic, lockless reads) ── */
    struct iosched_stats stats;
};

#endif /* __KERNEL_IOSCHED_TYPES_H */
