/**
 * @file iosched.c
 * @brief Block I/O scheduler — sorted dispatch with elevator (C-SCAN).
 *
 * Implements two policies:
 *
 *  NOOP   — First-in first-out.  Bios are dispatched in enqueue order.
 *  C-SCAN — Circular SCAN elevator.  Bios are dispatched in ascending
 *           sector order.  When the arm reaches the highest queued
 *           sector it wraps to the lowest.  Reads are given priority
 *           over writes (up to IOSCHED_WRITE_STARVE_LIMIT consecutive
 *           read batches before writes are forced).
 *
 * Bios are linked via their existing bio->list_entry field and kept in
 * per-direction sorted doubly-linked lists.  Insertion is O(n) in
 * queue depth, dispatch is O(1).
 *
 * The scheduler does NOT allocate memory — all state lives in the
 * struct iosched embedded in blkdev_t and in the bios themselves.
 */

#include <types.h>
#include <param.h>
#include <errno.h>
#include <defs.h>
#include <lock/spinlock.h>
#include <dev/bio.h>
#include <dev/blkdev.h>
#include <dev/iosched.h>
#include <list.h>
#include "printf.h"

/* ────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────── */

/**
 * __bio_sector - Extract the 512-byte sector number from a bio.
 *
 * This is just bio->blkno; we wrap it for readability.
 */
static inline uint64 __bio_sector(struct bio *bio)
{
    return bio->blkno;
}

/**
 * __queue_for_bio - Select the correct queue (read or write) for a bio.
 */
static inline list_node_t *__queue_for_bio(struct iosched *sched,
                                           struct bio *bio)
{
    return bio->rw ? &sched->write_queue : &sched->read_queue;
}

/**
 * __count_for_bio - Pointer to the counter for the bio's direction.
 */
static inline int *__count_for_bio(struct iosched *sched, struct bio *bio)
{
    return bio->rw ? &sched->nr_writes : &sched->nr_reads;
}

/* ────────────────────────────────────────────────────────────────────
 * Sorted insertion
 * ──────────────────────────────────────────────────────────────────── */

/**
 * __sorted_insert - Insert @bio into @queue in ascending sector order.
 *
 * Walks the queue from head→next (lowest sector) to find the first
 * entry whose sector is > bio->blkno and inserts before it.  If no
 * such entry exists the bio is appended at the tail (highest sector).
 *
 * O(n) in queue depth.  Queue depths are bounded by BSYNC_BATCH_MAX
 * (64) so this is fast in practice.
 */
static void __sorted_insert(list_node_t *queue, struct bio *bio)
{
    uint64 sector = __bio_sector(bio);
    list_node_t *pos;

    /* Walk forwards (ascending); find first entry > sector */
    list_foreach_entry(queue, pos) {
        struct bio *queued = container_of(pos, struct bio, list_entry);
        if (__bio_sector(queued) > sector) {
            /* Insert before this entry */
            list_entry_insert(LIST_PREV_ENTRY(pos), &bio->list_entry);
            return;
        }
    }

    /* All existing entries have sector <= ours — append at tail */
    list_entry_push(queue, &bio->list_entry);
}

/* ────────────────────────────────────────────────────────────────────
 * C-SCAN dispatch helpers
 * ──────────────────────────────────────────────────────────────────── */

/**
 * __cscan_pick - Pick the next bio from @queue using C-SCAN ordering.
 *
 * Finds the first bio with sector >= arm_position.  If none exists
 * (arm is past the highest queued sector), wraps to the lowest.
 *
 * Does NOT remove the bio from the queue or update counters.
 */
static struct bio *__cscan_pick(list_node_t *queue, uint64 arm_position)
{
    struct bio *wrap_candidate = NULL;
    list_node_t *pos;

    list_foreach_entry(queue, pos) {
        struct bio *bio = container_of(pos, struct bio, list_entry);
        if (wrap_candidate == NULL)
            wrap_candidate = bio;   /* lowest-sector entry (for wrap) */
        if (__bio_sector(bio) >= arm_position)
            return bio;
    }

    /* Wrap around to lowest sector */
    return wrap_candidate;
}

/**
 * __noop_pick - Pick the first (oldest) bio from @queue.
 */
static struct bio *__noop_pick(list_node_t *queue)
{
    if (LIST_IS_EMPTY(queue))
        return NULL;
    list_node_t *first = LIST_FIRST_ENTRY(queue);
    return container_of(first, struct bio, list_entry);
}

/**
 * __dequeue_bio - Remove a previously selected bio from its queue.
 */
static void __dequeue_bio(struct iosched *sched, struct bio *bio)
{
    list_entry_detach(&bio->list_entry);
    (*__count_for_bio(sched, bio))--;
}

/**
 * __select_queue - Choose which direction queue to drain next.
 *
 * C-SCAN: prefer reads unless we've dispatched IOSCHED_WRITE_STARVE_LIMIT
 * consecutive read batches and writes are pending.
 *
 * NOOP: prefer reads then writes (simple priority).
 */
static list_node_t *__select_queue(struct iosched *sched)
{
    bool reads_pending  = !LIST_IS_EMPTY(&sched->read_queue);
    bool writes_pending = !LIST_IS_EMPTY(&sched->write_queue);

    if (!reads_pending && !writes_pending)
        return NULL;

    if (sched->policy == IOSCHED_CSCAN) {
        /* Writes starving? Force a write batch. */
        if (writes_pending &&
            (!reads_pending ||
             sched->read_batch >= IOSCHED_WRITE_STARVE_LIMIT)) {
            sched->read_batch = 0;
            return &sched->write_queue;
        }
        if (reads_pending) {
            sched->read_batch++;
            return &sched->read_queue;
        }
        return &sched->write_queue;
    }

    /* NOOP: reads first */
    if (reads_pending)
        return &sched->read_queue;
    return &sched->write_queue;
}

/* ────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────── */

void iosched_init(struct iosched *sched, blkdev_t *bdev)
{
    spin_init(&sched->lock, "iosched");
    sched->bdev         = bdev;
    sched->policy       = IOSCHED_CSCAN;
    list_entry_init(&sched->read_queue);
    list_entry_init(&sched->write_queue);
    sched->nr_reads     = 0;
    sched->nr_writes    = 0;
    sched->arm_position = 0;
    sched->read_batch   = 0;
    __atomic_store_n(&sched->stats.enqueued,   0, __ATOMIC_RELAXED);
    __atomic_store_n(&sched->stats.dispatched, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sched->stats.requeued,   0, __ATOMIC_RELAXED);
}

void iosched_set_policy(struct iosched *sched, enum iosched_policy policy)
{
    spin_lock(&sched->lock);
    sched->policy = policy;
    spin_unlock(&sched->lock);
}

void iosched_enqueue(struct iosched *sched, struct bio *bio)
{
    list_entry_init(&bio->list_entry);

    spin_lock(&sched->lock);

    list_node_t *queue = __queue_for_bio(sched, bio);

    if (sched->policy == IOSCHED_CSCAN) {
        __sorted_insert(queue, bio);
    } else {
        /* NOOP: append at tail (FIFO) */
        list_entry_push(queue, &bio->list_entry);
    }
    (*__count_for_bio(sched, bio))++;
    __atomic_fetch_add(&sched->stats.enqueued, 1, __ATOMIC_RELAXED);

    spin_unlock(&sched->lock);
}

struct bio *iosched_dispatch(struct iosched *sched)
{
    struct bio *bio = NULL;

    spin_lock(&sched->lock);

    list_node_t *queue = __select_queue(sched);
    if (queue == NULL) {
        spin_unlock(&sched->lock);
        return NULL;
    }

    if (sched->policy == IOSCHED_CSCAN) {
        bio = __cscan_pick(queue, sched->arm_position);
    } else {
        bio = __noop_pick(queue);
    }

    if (bio == NULL) {
        spin_unlock(&sched->lock);
        return NULL;
    }

    __dequeue_bio(sched, bio);
    sched->arm_position = __bio_sector(bio);

    __atomic_fetch_add(&sched->stats.dispatched, 1, __ATOMIC_RELAXED);

    spin_unlock(&sched->lock);

    /* Submit to driver outside the scheduler lock */
    blkdev_submit_bio(sched->bdev, bio);
    return bio;
}

int iosched_dispatch_batch(struct iosched *sched, struct bio **out, int max)
{
    if (max <= 0)
        return 0;
    if (max > IOSCHED_BATCH_MAX)
        max = IOSCHED_BATCH_MAX;

    int n = 0;

    spin_lock(&sched->lock);

    while (n < max) {
        list_node_t *queue = __select_queue(sched);
        if (queue == NULL)
            break;

        struct bio *bio;
        if (sched->policy == IOSCHED_CSCAN) {
            bio = __cscan_pick(queue, sched->arm_position);
        } else {
            bio = __noop_pick(queue);
        }

        if (bio == NULL)
            break;

        __dequeue_bio(sched, bio);
        sched->arm_position = __bio_sector(bio);
        out[n++] = bio;
    }

    __atomic_fetch_add(&sched->stats.dispatched, n, __ATOMIC_RELAXED);

    spin_unlock(&sched->lock);

    /* Submit all to driver outside the scheduler lock, in order */
    for (int i = 0; i < n; i++) {
        blkdev_submit_bio(sched->bdev, out[i]);
    }

    return n;
}
