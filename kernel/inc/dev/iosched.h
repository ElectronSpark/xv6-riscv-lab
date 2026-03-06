/**
 * @file iosched.h
 * @brief Block I/O scheduler public API.
 *
 * Usage
 * -----
 * The scheduler is embedded in every blkdev_t and initialised at
 * device registration time.  There are two usage patterns:
 *
 * 1. **Batch path** (bsync / page-cache flusher):
 *    @code
 *    // Enqueue a batch of bios
 *    for (i = 0; i < n; i++)
 *        iosched_enqueue(&bdev->iosched, bios[i]);
 *
 *    // Dispatch all in elevator order — each bio is submitted to
 *    // the driver in sorted order.
 *    int ndispatched = iosched_dispatch_batch(&bdev->iosched, out, n);
 *
 *    // Await each dispatched bio
 *    for (i = 0; i < ndispatched; i++)
 *        bio_await(out[i]);
 *    @endcode
 *
 * 2. **Single-shot path** (bread / bwrite):
 *    Continue using blkdev_submit_bio() directly — adding a single
 *    bio to the scheduler and immediately dispatching it is equivalent
 *    to direct submission but with unnecessary overhead.
 */

#ifndef __KERNEL_IOSCHED_H
#define __KERNEL_IOSCHED_H

#include <dev/iosched_types.h>

/* ── Lifecycle ────────────────────────────────────────────────────── */

/**
 * iosched_init - Initialise an IO scheduler.
 * @sched:  Scheduler to initialise (typically &bdev->iosched).
 * @bdev:   The owning block device.
 *
 * Sets the default policy to IOSCHED_CSCAN.  Must be called before
 * any other iosched function.
 */
void iosched_init(struct iosched *sched, blkdev_t *bdev);

/**
 * iosched_set_policy - Change the scheduling algorithm.
 * @sched:  Target scheduler.
 * @policy: IOSCHED_NOOP or IOSCHED_CSCAN.
 *
 * Takes effect for the next dispatch; already-queued bios are not
 * re-sorted (they reside in sorted queues regardless of policy).
 */
void iosched_set_policy(struct iosched *sched, enum iosched_policy policy);

/* ── Enqueue ──────────────────────────────────────────────────────── */

/**
 * iosched_enqueue - Insert a bio into the scheduler's sorted queue.
 * @sched: Target scheduler.
 * @bio:   The bio to enqueue.  Must have bio->blkno and bio->rw set.
 *         Uses bio->list_entry for linkage; the caller must not
 *         reuse that field until the bio is dispatched.
 *
 * The bio is inserted into the read or write queue in ascending
 * sector (bio->blkno) order.  O(n) insertion where n is the current
 * queue depth.
 */
void iosched_enqueue(struct iosched *sched, struct bio *bio);

/* ── Dispatch ─────────────────────────────────────────────────────── */

/**
 * iosched_dispatch - Pick and dispatch the next bio.
 * @sched: Target scheduler.
 *
 * Selects the next bio according to the current policy, removes it
 * from the scheduler queue, and submits it to the driver via
 * bdev->ops.submit_bio().
 *
 * Returns the dispatched bio (caller should bio_await()), or NULL
 * if both queues are empty.
 */
struct bio *iosched_dispatch(struct iosched *sched);

/**
 * iosched_dispatch_batch - Dispatch up to @max bios in order.
 * @sched: Target scheduler.
 * @out:   Output array of dispatched bio pointers.
 * @max:   Maximum number of bios to dispatch.
 *
 * Each dispatched bio is submitted to the driver before the next one
 * is selected so that the driver sees them in scheduler order.
 *
 * Returns the number of bios written into @out[].
 */
int iosched_dispatch_batch(struct iosched *sched, struct bio **out, int max);

/* ── Query ────────────────────────────────────────────────────────── */

/**
 * iosched_nr_pending - Number of bios waiting for dispatch.
 */
static inline int iosched_nr_pending(struct iosched *sched)
{
    return sched->nr_reads + sched->nr_writes;
}

/**
 * iosched_get_stats - Snapshot scheduler statistics.
 * @sched: Target scheduler.
 * @out:   Destination for the snapshot.
 */
static inline void iosched_get_stats(struct iosched *sched,
                                     struct iosched_stats *out)
{
    out->enqueued   = __atomic_load_n(&sched->stats.enqueued,   __ATOMIC_RELAXED);
    out->dispatched = __atomic_load_n(&sched->stats.dispatched, __ATOMIC_RELAXED);
    out->requeued   = __atomic_load_n(&sched->stats.requeued,   __ATOMIC_RELAXED);
}

#endif /* __KERNEL_IOSCHED_H */
