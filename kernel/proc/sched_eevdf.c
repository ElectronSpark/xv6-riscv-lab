/**
 * @file sched_eevdf.c
 * @brief EEVDF (Earliest Eligible Virtual Deadline First) scheduler class.
 *
 * This scheduler occupies major priority levels
 * [EEVDF_MAJOR_PRIORITY_START, EEVDF_MAJOR_PRIORITY_LIMIT) and provides
 * proportional-share CPU allocation via virtual runtimes and virtual
 * deadlines keyed by per-entity weights.
 *
 * Key design decisions (MVP):
 *  - Weights are derived from the existing priority field: higher minor
 *    priority => lower weight (more CPU).  The major priority selects the
 *    EEVDF class instance; minor 0‥3 maps to weight multipliers.
 *  - Entities are stored in a red-black tree keyed by virtual deadline.
 *  - Eligible entity = one whose vruntime <= rq min_vruntime (i.e. the
 *    entity has not run "ahead" of the rq).
 *  - pick_next selects the leftmost eligible entity (smallest deadline
 *    among those that are eligible).
 *  - task_tick updates runtime accounting and requests reschedule when
 *    the current entity has exhausted its ideal runtime slice.
 *  - Wakeup preemption: a newly enqueued entity preempts current if its
 *    virtual deadline is earlier (conservative: same-CPU only via
 *    SET_NEEDS_RESCHED).
 *
 * Load balancing strategy (multi-mode):
 *  - **EWMA load tracking**: each eevdf_rq tracks load_sum (instantaneous
 *    sum of entity weights) and load_avg (exponentially weighted moving
 *    average with ~50ms half-life) for stable imbalance detection.
 *  - **Smart wakeup placement**: select_task_rq considers idle CPUs first,
 *    then picks the least-loaded CPU by weighted load with a locality
 *    bias towards the current CPU.
 *  - **Periodic pull-based rebalancing** (Mode A): during task_tick,
 *    busy CPUs periodically scan for imbalance. Uses tiered intervals:
 *    fast (10ms) when lightly loaded, normal (100ms) otherwise.
 *    Exponential backoff (up to 8× interval) on consecutive failures.
 *    Can pull up to 3 entities per balance pass with smart entity
 *    selection that targets the desired imbalance amount.
 *  - **Idle CPU proactive balancing** (Mode B): when pick_next_task
 *    finds an empty rq, aggressively tries to pull work from the
 *    busiest CPUs. Tries up to 4 candidate CPUs (sorted by load),
 *    uses try-locks, and accepts any migratable entity. Has a ~1ms
 *    cooldown to avoid spinning on failed attempts.
 *  - **Migration mechanics**: dequeues from source rq + enqueues on
 *    destination rq, adjusting vruntime to destination min_vruntime.
 *    Uses rq_trylock() for remote CPUs to avoid deadlock. Sends
 *    IPI_REASON_RESCHEDULE to source CPU after migration.
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "proc_private.h"
#include "defs.h"
#include "printf.h"
#include "rbtree.h"
#include <mm/slab.h>
#include <mm/page.h>
#include <mm/vm.h>
#include <smp/percpu.h>
#include "bits.h"
#include "errno.h"
#include "bintree.h"
#include "smp/ipi.h"

/* ──────────────────────────────────────────────────────────────
 *  Weight table — maps the 40 EEVDF priorities to Linux-compatible
 *  weights, mirroring the sched_prio_to_weight[] table from Linux
 *  kernel/sched/core.c.
 *
 *  EEVDF priority 80 (major 20, minor 0)  → index 0  → nice -20
 *  EEVDF priority 100 (major 25, minor 0) → index 20 → nice  0
 *  EEVDF priority 119 (major 29, minor 3) → index 39 → nice +19
 *
 *  Adjacent nice levels differ by ~1.25×, so a +1 nice step
 *  yields ~10% less CPU share.
 * ────────────────────────────────────────────────────────────── */

#define EEVDF_NUM_PRIOS 40 /* Total number of EEVDF priority levels */

/*
 * sched_prio_to_weight[]: lifted from Linux — maps nice value to
 * entity weight.  nice 0 → weight 1024 (NICE_0_WEIGHT).
 */
static const uint32 __eevdf_weight_table[EEVDF_NUM_PRIOS] = {
    /* nice -20 */ 88761, 71755, 56483, 46273, 36291,
    /* nice -15 */ 29154, 23254, 18705, 14949, 11916,
    /* nice -10 */  9548,  7620,  6100,  4904,  3906,
    /* nice  -5 */  3121,  2501,  1991,  1586,  1277,
    /* nice   0 */  1024,   820,   655,   526,   423,
    /* nice   5 */   335,   272,   215,   172,   137,
    /* nice  10 */   110,    87,    70,    56,    45,
    /* nice  15 */    36,    29,    23,    18,    15,
};

/*
 * sched_prio_to_wmult[]: inverse weights for fixed-point vruntime
 * computation.  inv_weight ≈ 2^WMULT_SHIFT / weight.
 *
 * Allows: delta_vrt = delta_exec * NICE_0_WEIGHT * inv_weight >> WMULT_SHIFT
 */
#define EEVDF_WMULT_SHIFT 32

static const uint32 __eevdf_inv_weight_table[EEVDF_NUM_PRIOS] = {
    /* nice -20 */     48388,     59856,     76040,     92818,    118348,
    /* nice -15 */    147320,    184698,    229616,    287308,    360437,
    /* nice -10 */    449829,    563644,    704093,    875809,   1099582,
    /* nice  -5 */   1376151,   1717300,   2157191,   2708050,   3363326,
    /* nice   0 */   4194304,   5237765,   6557202,   8165337,  10153587,
    /* nice   5 */  12820798,  15790321,  19976592,  24970740,  31350126,
    /* nice  10 */  39045157,  49367440,  61356676,  76695844,  95443717,
    /* nice  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};

/* ──────────────────────────────────────────────────────────────
 *  Helper: weighted virtual-time computation (fixed-point)
 * ────────────────────────────────────────────────────────────── */

/*
 * Compute virtual time delta:
 *   delta_vrt = delta_exec * NICE_0_WEIGHT / weight
 *
 * Using fixed-point arithmetic (same approach as Linux CFS/EEVDF):
 *   delta_vrt = (delta_exec * NICE_0_WEIGHT * inv_weight) >> WMULT_SHIFT
 *
 * where inv_weight ≈ (1 << WMULT_SHIFT) / weight.
 *
 * We use a 128-bit intermediate product to avoid overflow or
 * precision loss that the Linux 64-bit-only path suffers for
 * high-weight (negative-nice) entities.
 */
#define NICE_0_WEIGHT 1024U

static inline int64 __calc_delta_vruntime(uint64 delta_exec,
                                          struct load_weight *lw) {
    if (lw->weight == NICE_0_WEIGHT) {
        return (int64)delta_exec;
    }

    /*
     * fact = NICE_0_WEIGHT * inv_weight
     *      ≈ NICE_0_WEIGHT * (2^32 / weight)
     *      = NICE_0_WEIGHT * 2^32 / weight
     *
     * result = delta_exec * fact >> WMULT_SHIFT
     *        ≈ delta_exec * NICE_0_WEIGHT / weight             ✓
     *
     * The product delta_exec * fact can exceed 64 bits (e.g., for
     * nice -20: fact ≈ 2^25.5, delta_exec can be 2^27 for 100ms
     * at 1GHz → product ≈ 2^52.5, which is fine.  But for safety
     * with larger deltas we use 128-bit multiply).
     */
    __uint128_t product = (__uint128_t)delta_exec * NICE_0_WEIGHT;
    product *= (uint64)lw->inv_weight;
    return (int64)(product >> EEVDF_WMULT_SHIFT);
}

/* ──────────────────────────────────────────────────────────────
 *  Per-cls_id static storage for eevdf_rq arrays (one per CPU).
 * ────────────────────────────────────────────────────────────── */
static struct eevdf_rq *__eevdf_rqs[PRIORITY_MAINLEVELS];

/* ──────────────────────────────────────────────────────────────
 *  Red-black tree callbacks — key is virtual deadline.
 * ────────────────────────────────────────────────────────────── */
static int __eevdf_keys_cmp(uint64 k1, uint64 k2) {
    int64 diff = (int64)k1 - (int64)k2;
    if (diff < 0) return -1;
    if (diff > 0) return 1;
    return 0;
}

static uint64 __eevdf_get_key(struct rb_node *node) {
    struct sched_entity *se = rb_entry(node, struct sched_entity, rb_entry);
    return (uint64)se->deadline;
}

static struct rb_root_opts __eevdf_rb_opts = {
    .keys_cmp_fun = __eevdf_keys_cmp,
    .get_key_fun = __eevdf_get_key,
};

/* ──────────────────────────────────────────────────────────────
 *  Internal helpers
 * ────────────────────────────────────────────────────────────── */

static inline struct eevdf_rq *__to_eevdf_rq(struct rq *rq) {
    return container_of(rq, struct eevdf_rq, rq);
}

/* Update min_vruntime to be the max of current min_vruntime and the
 * leftmost entity's vruntime (ensures monotonic progress). */
static void __update_min_vruntime(struct eevdf_rq *erq) {
    int64 vruntime = erq->min_vruntime;

    struct rb_node *leftmost = rb_first_node(&erq->tasks_timeline);
    if (leftmost) {
        struct sched_entity *se =
            rb_entry(leftmost, struct sched_entity, rb_entry);
        if (se->vruntime > vruntime) {
            vruntime = se->vruntime;
        }
    }

    /* min_vruntime must only go forward */
    if (vruntime > erq->min_vruntime) {
        erq->min_vruntime = vruntime;
    }
}

/* Place an entity in the rb-tree.
 * Handles duplicate deadline keys by nudging the deadline +1 on collision. */
static void __enqueue_entity(struct eevdf_rq *erq, struct sched_entity *se) {
    rb_node_init(&se->rb_entry);
    struct rb_node *result = rb_insert_color(&erq->tasks_timeline,
                                             &se->rb_entry);
    /* If key collision (same deadline), nudge deadline and retry */
    while (result != &se->rb_entry) {
        se->deadline++;
        rb_node_init(&se->rb_entry);
        result = rb_insert_color(&erq->tasks_timeline, &se->rb_entry);
    }
    se->eevdf_on_rq = 1;
    erq->nr_running++;
}

static void __dequeue_entity(struct eevdf_rq *erq, struct sched_entity *se) {
    if (!se->eevdf_on_rq) {
        return;
    }
    rb_delete_node_color(&erq->tasks_timeline, &se->rb_entry);
    rb_node_init(&se->rb_entry);
    se->eevdf_on_rq = 0;
    erq->nr_running--;
}

/* Set entity weight from its full EEVDF priority.
 * The index into the 40-entry weight table is computed from the raw priority
 * value: idx = se->priority - EEVDF_PRIORITY_START, clamped to [0, 39].
 *
 * Priority mapping:
 *   EEVDF priority 80  (major 20 minor 0) → idx  0 → nice -20 → weight 88761
 *   EEVDF priority 100 (major 25 minor 0) → idx 20 → nice   0 → weight  1024
 *   EEVDF priority 119 (major 29 minor 3) → idx 39 → nice +19 → weight    15
 */
static void __set_load_weight(struct sched_entity *se) {
    int idx = se->priority - EEVDF_PRIORITY_START;
    if (idx < 0) idx = 0;
    if (idx >= EEVDF_NUM_PRIOS) idx = EEVDF_NUM_PRIOS - 1;
    se->load.weight = __eevdf_weight_table[idx];
    se->load.inv_weight = __eevdf_inv_weight_table[idx];
}

/* Place a new/waking entity: set its vruntime based on rq min_vruntime
 * and compute the initial virtual deadline. */
static void __place_entity(struct eevdf_rq *erq, struct sched_entity *se,
                           int initial) {
    int64 vruntime = erq->min_vruntime;

    if (!initial) {
        /* Returning from sleep — credit half a slice worth of lag so
         * sleepers don't get unfairly delayed. */
        int64 half_slice = __calc_delta_vruntime(se->slice / 2, &se->load);
        if (vruntime > half_slice) {
            vruntime -= half_slice;
        } else {
            vruntime = 0;
        }
    }

    if (!initial && se->vruntime > vruntime) {
        /*
         * A task that slept after a burst of CPU time can have a vruntime far
         * ahead of the runqueue. Keeping that full lead makes interactive
         * sleepers wait behind CPU-bound work after wakeup. Preserve short-term
         * fairness, but do not let a sleeper remain ahead of the runqueue
         * minimum after wakeup. Chrome's audio/UI workers often sleep after
         * short CPU bursts; leaving them one full slice ahead can bury futex
         * wakeups behind a large worker pool and produce visible stalls.
         */
        int64 max_ahead = erq->min_vruntime;
        vruntime = se->vruntime > max_ahead ? max_ahead : se->vruntime;
    }

    se->vruntime = vruntime;
    se->deadline = vruntime + __calc_delta_vruntime(se->slice, &se->load);
    se->min_vruntime = erq->min_vruntime;
}

/* ──────────────────────────────────────────────────────────────
 *  Forward declarations for load balancing & PELT (defined below)
 * ────────────────────────────────────────────────────────────── */
static void __update_load_avg(struct eevdf_rq *erq);
static void __update_curr_pelt(struct eevdf_rq *erq, struct sched_entity *se);
static void __update_entity_pelt(struct sched_entity *se, int running);
static inline void __update_entity_load_contrib(struct sched_entity *se);
static inline void __add_entity_load_to_cpu(struct eevdf_rq *erq,
                                            struct sched_entity *se);
static inline void __remove_entity_load_from_cpu(struct eevdf_rq *erq,
                                                 struct sched_entity *se);
static int __eevdf_idle_balance(struct eevdf_rq *this_erq, int this_cpu,
                                int cls_id);
static void __eevdf_periodic_balance(struct eevdf_rq *this_erq, int this_cpu,
                                     int cls_id);

/* Update runtime accounting for the currently running entity. */
static void __update_curr(struct eevdf_rq *erq, struct sched_entity *se) {
    uint64 now = r_time();
    uint64 delta_exec = now - se->exec_start;
    if ((int64)delta_exec <= 0) {
        return;
    }
    se->exec_start = now;
    se->sum_exec_runtime += delta_exec;

    int64 delta_vrt = __calc_delta_vruntime(delta_exec, &se->load);
    se->vruntime += delta_vrt;

    /* Update per-entity PELT (entity is running). */
    __update_curr_pelt(erq, se);

    __update_min_vruntime(erq);
}

/* ──────────────────────────────────────────────────────────────
 *  sched_class callbacks
 * ────────────────────────────────────────────────────────────── */

static void __eevdf_enqueue_task(struct rq *rq, struct sched_entity *se) {
    int rq_cpu = rq_cpu_id(rq);
    assert(rq_cpu >= 0, "__eevdf_enqueue_task: rq cpu resolution failed");
    struct eevdf_rq *erq = __to_eevdf_rq(rq);
    __set_load_weight(se);

    /* PELT: decay util while the entity was sleeping, then recompute
     * its weighted load contribution before adding to CPU aggregates. */
    __update_entity_pelt(se, 0); /* sleeping → decay toward 0 */
    __update_entity_load_contrib(se);

    __place_entity(erq, se, (se->vruntime == 0 && se->deadline == 0));
    __enqueue_entity(erq, se);
    __update_min_vruntime(erq);

    /* Track weighted load for load balancing. */
    erq->load_sum += se->load.weight;
    __add_entity_load_to_cpu(erq, se);
    __update_load_avg(erq);

    /* Wakeup preemption check: if the new entity has an earlier deadline
     * than the currently running entity on this CPU, request reschedule. */
    if (rq_cpu == cpuid()) {
        struct sched_entity *curr_se = current->sched_entity;
        if (curr_se != NULL && IS_EEVDF_PRIORITY(curr_se->priority)) {
            if (se->deadline < curr_se->deadline) {
                SET_NEEDS_RESCHED();
            }
        }
    }
}

static void __eevdf_dequeue_task(struct rq *rq, struct sched_entity *se) {
    struct eevdf_rq *erq = __to_eevdf_rq(rq);

    /* If the entity is the currently running one (set_next removed it from
     * the tree already), just update accounting. */
    if (se->eevdf_on_rq) {
        __dequeue_entity(erq, se);
    }

    /* Remove weight from load tracking. */
    if (erq->load_sum >= se->load.weight) {
        erq->load_sum -= se->load.weight;
    } else {
        erq->load_sum = 0;
    }
    __remove_entity_load_from_cpu(erq, se);
    __update_load_avg(erq);

    __update_min_vruntime(erq);
}

/*
 * pick_next_task — EEVDF: pick the entity with the earliest virtual
 * deadline among those whose vruntime <= rq's min_vruntime (eligible).
 *
 * Simplified: walk the tree from leftmost (smallest deadline) and pick
 * the first entity whose vruntime <= min_vruntime.  Because most entities
 * should be eligible this is usually the leftmost node itself.
 */
static struct sched_entity *__eevdf_pick_next_task(struct rq *rq) {
    int rq_cpu = rq_cpu_id(rq);
    assert(rq_cpu >= 0, "__eevdf_pick_next_task: rq cpu resolution failed");
    struct eevdf_rq *erq = __to_eevdf_rq(rq);

    /* If the local rq is empty, try idle balance first to pull work
     * from a busier CPU before this CPU falls through to idle. */
    if (erq->nr_running == 0) {
        /* Attempt idle balance — returns 1 if work was pulled. */
        if (!__eevdf_idle_balance(erq, rq_cpu, rq->class_id)) {
            return NULL;
        }
        /* Fall through — we just pulled at least one entity. */
    }

    int64 min_vrt = erq->min_vruntime;
    struct rb_node *node;
    struct sched_entity *best = NULL;

    /* Walk from smallest deadline.  Pick first eligible. */
    for (node = rb_first_node(&erq->tasks_timeline); node != NULL;
         node = rb_next_node(node)) {
        struct sched_entity *se =
            rb_entry(node, struct sched_entity, rb_entry);
        if (se->vruntime <= min_vrt) {
            best = se;
            break;
        }
    }

    /* Fallback: if no eligible entity, pick the leftmost (earliest
     * deadline, even if slightly ahead of min_vruntime). This prevents
     * starvation when all entities are temporarily ineligible. */
    if (best == NULL) {
        best = rb_entry_safe(rb_first_node(&erq->tasks_timeline),
                             struct sched_entity, rb_entry);
    }

    return best;
}

static void __eevdf_set_next_task(struct rq *rq, struct sched_entity *se) {
    int rq_cpu = rq_cpu_id(rq);
    assert(rq_cpu >= 0, "__eevdf_set_next_task: rq cpu resolution failed");
    struct eevdf_rq *erq = __to_eevdf_rq(rq);
    /* Remove entity from the tree while it is running. */
    __dequeue_entity(erq, se);
    se->exec_start = r_time();

    /* If this was the last entity in the tree, clear ready bits. */
    if (erq->nr_running == 0) {
        rq_clear_ready(rq->class_id, rq_cpu);
    }
}

static void __eevdf_put_prev_task(struct rq *rq, struct sched_entity *se) {
    int rq_cpu = rq_cpu_id(rq);
    assert(rq_cpu >= 0, "__eevdf_put_prev_task: rq cpu resolution failed");
    struct eevdf_rq *erq = __to_eevdf_rq(rq);

    /* Update accounting for the time the entity spent running. */
    __update_curr(erq, se);

    /* Re-insert entity into tree. */
    if (!se->eevdf_on_rq) {
        /* Recompute deadline if the entity exhausted its current slice. */
        if (se->vruntime >= se->deadline) {
            se->deadline = se->vruntime +
                           __calc_delta_vruntime(se->slice, &se->load);
        }
        __enqueue_entity(erq, se);
    }

    /* Mark rq ready since we just added an entity. */
    rq_set_ready(rq->class_id, rq_cpu);
}

static void __eevdf_task_tick(struct rq *rq, struct sched_entity *se) {
    int rq_cpu = rq_cpu_id(rq);
    assert(rq_cpu >= 0, "__eevdf_task_tick: rq cpu resolution failed");
    struct eevdf_rq *erq = __to_eevdf_rq(rq);
    __update_curr(erq, se);

    /* If the current entity has reached its virtual deadline, request
     * a reschedule so pick_next can choose the best candidate. */
    if (se->vruntime >= se->deadline) {
        SET_NEEDS_RESCHED();
        /* Recompute deadline for the next scheduling round. */
        se->deadline = se->vruntime +
                       __calc_delta_vruntime(se->slice, &se->load);
    }

    /* Periodic load balancing: attempt to pull tasks from busier CPUs. */
    __eevdf_periodic_balance(erq, rq_cpu, rq->class_id);
}

static void __eevdf_task_fork(struct rq *rq, struct sched_entity *se) {
    struct eevdf_rq *erq = __to_eevdf_rq(rq);
    /* Child inherits parent's weight via priority. Set initial vruntime to
     * the rq's current min_vruntime so it doesn't starve old tasks. */
    __set_load_weight(se);
    se->vruntime = erq->min_vruntime;
    se->slice = EEVDF_DEFAULT_SLICE_TICKS;
    se->deadline = se->vruntime +
                   __calc_delta_vruntime(se->slice, &se->load);

    /* Initialise PELT for newly forked entity. */
    se->util_avg = 0;
    se->pelt_stamp = r_time();
    __update_entity_load_contrib(se);
}

static void __eevdf_task_dead(struct rq *rq, struct sched_entity *se) {
    struct eevdf_rq *erq = __to_eevdf_rq(rq);
    if (se->eevdf_on_rq) {
        __dequeue_entity(erq, se);
    }
    /* Remove weight (if still counted — dequeue_task normally handles this,
     * but task_dead may be called in special paths). */
    if (erq->load_sum >= se->load.weight) {
        erq->load_sum -= se->load.weight;
    } else {
        erq->load_sum = 0;
    }
    __remove_entity_load_from_cpu(erq, se);
    __update_min_vruntime(erq);
}

static void __eevdf_yield_task(struct rq *rq) {
    struct sched_entity *se = current->sched_entity;
    struct eevdf_rq *erq = __to_eevdf_rq(rq);

    /* Advance vruntime to the deadline to give away the rest of the
     * time slice, then request reschedule. */
    __update_curr(erq, se);
    if (se->vruntime < se->deadline) {
        se->vruntime = se->deadline;
    }
    se->deadline = se->vruntime +
                   __calc_delta_vruntime(se->slice, &se->load);
    SET_NEEDS_RESCHED();
}

/* ──────────────────────────────────────────────────────────────
 *  Load balancing: multi-mode pull-based rebalancer
 *
 *  Three modes of operation:
 *
 *  (A) PERIODIC BALANCE — driven by task_tick on busy CPUs.
 *      Uses tiered intervals: fast (10ms) when severely imbalanced,
 *      normal (100ms) otherwise.  Exponential backoff on consecutive
 *      failures to avoid wasting cycles on un-pullable loads.
 *
 *  (B) IDLE BALANCE — triggered from pick_next_task when the local
 *      rq is empty.  Very aggressive: low threshold, tries multiple
 *      candidate CPUs, accepts any migratable entity.
 *
 *  (C) EWMA LOAD TRACKING — load_avg is an exponential weighted
 *      moving average of load_sum, updated lazily. Used for periodic
 *      balance decisions to avoid reacting to transient spikes.
 *
 *  (D) PER-ENTITY LOAD TRACKING (PELT) — each sched_entity tracks
 *      util_avg (CPU utilization ratio, 0..1024) and load_avg_contrib
 *      (weight × util_avg >> 10).  The per-CPU aggregates cpu_util
 *      and cpu_load are derived from the sum of entity contributions.
 *      This allows the balancer to distinguish CPU-bound tasks from
 *      sleeping ones, making migration decisions far more accurate
 *      than static weight sums alone.
 * ────────────────────────────────────────────────────────────── */

/* Balance intervals in raw timer ticks (tuned for ~1GHz timebase). */
#define EEVDF_BALANCE_INTERVAL_NORMAL  (100000000ULL)  /* ~100ms */
#define EEVDF_BALANCE_INTERVAL_FAST     (10000000ULL)  /*  ~10ms */
#define EEVDF_IDLE_COOLDOWN              (1000000ULL)  /*   ~1ms */

/* EWMA half-life period in raw ticks (~50ms). */
#define EEVDF_LOAD_AVG_PERIOD           (50000000ULL)

/* PELT half-life period in raw ticks (~32ms, same as Linux). */
#define EEVDF_PELT_PERIOD               (32000000ULL)

/* Imbalance thresholds. */
#define EEVDF_IMBALANCE_MIN            (NICE_0_WEIGHT)       /* 1024 */
#define EEVDF_IMBALANCE_SEVERE         (NICE_0_WEIGHT * 4)   /* 4096 */

/* Maximum entities to pull in a single periodic balance pass. */
#define EEVDF_MAX_PULL_PER_BALANCE     3

/* Maximum remote CPUs to try-lock during idle balance. */
#define EEVDF_IDLE_MAX_TRYLOCK         4

/* ── EWMA load average helpers ─────────────────────────────── */

/**
 * __update_load_avg - Lazily update the EWMA load average.
 *
 * Uses a power-of-two approximation of exponential decay:
 *   load_avg = load_avg * decay + load_sum * (1 - decay)
 * where decay ≈ elapsed / PERIOD, clamped to [0, 1].
 *
 * To avoid floating point:
 *   load_avg = (load_avg * (PERIOD - elapsed) + load_sum * elapsed) / PERIOD
 * with elapsed clamped at PERIOD.
 */
static void __update_load_avg(struct eevdf_rq *erq) {
    uint64 now = r_time();
    uint64 elapsed = now - erq->load_avg_stamp;
    if (elapsed < EEVDF_LOAD_AVG_PERIOD / 8) {
        return; /* Too soon — skip to amortise cost. */
    }
    erq->load_avg_stamp = now;

    if (elapsed > EEVDF_LOAD_AVG_PERIOD) {
        elapsed = EEVDF_LOAD_AVG_PERIOD; /* Clamp so decay doesn't go negative. */
    }

    uint64 complement = EEVDF_LOAD_AVG_PERIOD - elapsed;
    erq->load_avg = (erq->load_avg * complement +
                     erq->load_sum * elapsed) /
                    EEVDF_LOAD_AVG_PERIOD;
}

/* ── Per-Entity Load Tracking (PELT) ──────────────────────── */

/**
 * __update_entity_load_contrib - Recompute entity's weighted load
 *   contribution from its current util_avg and weight.
 *
 *   load_avg_contrib = weight * util_avg >> SCHED_FIXEDPOINT_SHIFT
 *
 * This represents the entity's actual CPU demand scaled by its
 * scheduling weight (priority).
 */
static inline void __update_entity_load_contrib(struct sched_entity *se) {
    se->load_avg_contrib =
        ((uint64)se->load.weight * se->util_avg) >> SCHED_FIXEDPOINT_SHIFT;
}

/**
 * __update_entity_pelt - Update per-entity utilization tracking.
 *
 * Called during __update_curr (entity is running) and at
 * enqueue/dequeue boundaries (entity was sleeping).
 *
 * @se:      the sched_entity to update
 * @running: 1 if entity is/was running, 0 if it was sleeping
 *
 * Uses exponential decay with ~32ms half-life (EEVDF_PELT_PERIOD):
 *   When running:  util_avg moves toward SCHED_FIXEDPOINT_ONE (1024)
 *   When sleeping: util_avg decays toward 0
 *
 * Fixed-point arithmetic avoids floating point:
 *   util_avg = (util_avg * (PERIOD - elapsed) + target * elapsed) / PERIOD
 */
static void __update_entity_pelt(struct sched_entity *se, int running) {
    uint64 now = r_time();
    uint64 elapsed;

    if (se->pelt_stamp == 0) {
        /* First update — just stamp and return. */
        se->pelt_stamp = now;
        return;
    }

    elapsed = now - se->pelt_stamp;
    if (elapsed < EEVDF_PELT_PERIOD / 16) {
        return; /* Too soon — skip to amortise cost. */
    }
    se->pelt_stamp = now;

    if (elapsed > EEVDF_PELT_PERIOD) {
        elapsed = EEVDF_PELT_PERIOD; /* Clamp to one full period. */
    }

    uint64 complement = EEVDF_PELT_PERIOD - elapsed;
    uint32 target = running ? SCHED_FIXEDPOINT_ONE : 0;

    se->util_avg = (uint32)(((uint64)se->util_avg * complement +
                             (uint64)target * elapsed) /
                            EEVDF_PELT_PERIOD);

    __update_entity_load_contrib(se);
}

/**
 * __add_entity_load_to_cpu - Add entity's PELT contribution to per-CPU
 *   aggregates.  Called when entity is enqueued / arrives on CPU.
 */
static inline void __add_entity_load_to_cpu(struct eevdf_rq *erq,
                                            struct sched_entity *se) {
    erq->cpu_util += se->util_avg;
    erq->cpu_load += se->load_avg_contrib;
}

/**
 * __remove_entity_load_from_cpu - Remove entity's PELT contribution
 *   from per-CPU aggregates.  Called when entity is dequeued / leaves CPU.
 */
static inline void __remove_entity_load_from_cpu(struct eevdf_rq *erq,
                                                 struct sched_entity *se) {
    if (erq->cpu_util >= se->util_avg)
        erq->cpu_util -= se->util_avg;
    else
        erq->cpu_util = 0;

    if (erq->cpu_load >= se->load_avg_contrib)
        erq->cpu_load -= se->load_avg_contrib;
    else
        erq->cpu_load = 0;
}

/**
 * __update_curr_pelt - Update the running entity's PELT and refresh
 *   per-CPU aggregates.  Called from __update_curr (on every tick).
 */
static void __update_curr_pelt(struct eevdf_rq *erq, struct sched_entity *se) {
    /* Snapshot old contribution before updating. */
    uint64 old_contrib = se->load_avg_contrib;
    uint32 old_util = se->util_avg;

    __update_entity_pelt(se, 1); /* running = 1 */

    /* Update per-CPU aggregates with the delta. */
    int64 delta_util = (int64)se->util_avg - (int64)old_util;
    int64 delta_load = (int64)se->load_avg_contrib - (int64)old_contrib;

    if (delta_util > 0)
        erq->cpu_util += (uint64)delta_util;
    else if (delta_util < 0) {
        uint64 dec = (uint64)(-delta_util);
        erq->cpu_util = erq->cpu_util > dec ? erq->cpu_util - dec : 0;
    }

    if (delta_load > 0)
        erq->cpu_load += (uint64)delta_load;
    else if (delta_load < 0) {
        uint64 dec = (uint64)(-delta_load);
        erq->cpu_load = erq->cpu_load > dec ? erq->cpu_load - dec : 0;
    }
}

/* ── Entity selection for migration ──────────────────────────
 *
 * __pick_migrate_entity - Walk the source rb-tree and pick the best
 *   entity to migrate.
 *
 * Selection criteria (priority order):
 *  1. Must be in the tree (eevdf_on_rq == 1).
 *  2. Affinity must allow the destination CPU.
 *  3. Among candidates, prefer the entity whose weight best matches
 *     the target imbalance (avoid over-correcting).
 *  4. Tie-break: prefer higher vruntime (has had its share).
 *
 * @src_erq:     source eevdf_rq (locked)
 * @dst_cpu:     destination CPU id
 * @target_load: desired amount of load to move (may pull more or less)
 * @max_pull:    maximum number of entities to select
 * @out:         output array of entities to migrate (caller-supplied)
 *
 * Returns: number of entities placed in @out.
 */
static int __pick_migrate_entities(struct eevdf_rq *src_erq, int dst_cpu,
                                   uint64 target_load, int max_pull,
                                   struct sched_entity **out) {
    int count = 0;
    uint64 pulled_load = 0;

    struct rb_node *node;
    for (node = rb_first_node(&src_erq->tasks_timeline);
         node != NULL && count < max_pull;
         node = rb_next_node(node)) {
        struct sched_entity *se =
            rb_entry(node, struct sched_entity, rb_entry);

        if (!se->eevdf_on_rq) continue;
        if (!rq_cpu_allowed(se, dst_cpu)) continue;

        out[count++] = se;
        /* Use entity's PELT load contribution for target matching.
         * Falls back to static weight if PELT hasn't ramped up yet. */
        uint64 entity_load = se->load_avg_contrib;
        if (entity_load == 0) entity_load = se->load.weight;
        pulled_load += entity_load;

        /* Stop once we've gathered enough load. */
        if (pulled_load >= target_load) break;
    }

    return count;
}

/**
 * __do_migrate - Dequeue an entity from source rq and enqueue on dest rq.
 *
 * Both rq locks must be held by caller.
 * Adjusts vruntime to destination min_vruntime for fairness.
 */
static void __do_migrate(struct eevdf_rq *src_erq, struct eevdf_rq *dst_erq,
                         struct sched_entity *se) {
    rq_dequeue_task(&src_erq->rq, se);

    /* Adjust vruntime so the migrated entity neither starves nor
     * jumps ahead on the destination rq. */
    se->vruntime = dst_erq->min_vruntime;
    se->deadline = se->vruntime +
                   __calc_delta_vruntime(se->slice, &se->load);

    rq_enqueue_task(&dst_erq->rq, se);

    /* Note: per-CPU cpu_load/cpu_util are adjusted automatically by
     * the dequeue (removes from source) and enqueue (adds to dest)
     * callbacks above. No explicit adjustment needed here. */
}

/* ── Scan helpers ─────────────────────────────────────────── */

/**
 * __find_busiest_cpu - Scan all CPUs for the same cls_id and return
 *   the CPU with the highest load_avg.
 *
 * Reads are racy (no remote locks) — caller re-validates under lock.
 */
static int __find_busiest_cpu(int this_cpu, int cls_id,
                              uint64 *out_busiest_load) {
    int busiest_cpu = -1;
    uint64 busiest_load = 0;

    for (int cpu = 0; cpu < cpu_possible_count(); cpu++) {
        if (cpu == this_cpu) continue;
        struct eevdf_rq *remote = &__eevdf_rqs[cls_id][cpu];
        /* Use PELT-based cpu_load (weighted utilization) as primary
         * metric.  Falls back to load_sum for responsiveness if the
         * PELT signal hasn't had time to ramp up yet. */
        uint64 rload = smp_load_acquire(&remote->cpu_load);
        uint64 rload_sum = smp_load_acquire(&remote->load_sum);
        if (rload_sum > rload) rload = rload_sum;
        if (rload > busiest_load) {
            busiest_load = rload;
            busiest_cpu = cpu;
        }
    }

    *out_busiest_load = busiest_load;
    return busiest_cpu;
}

/**
 * __compute_imbalance - Given busiest and local loads, compute how
 *   much load to pull and whether it's worth doing.
 *
 * Returns target load to pull, or 0 if not worth balancing.
 */
static uint64 __compute_imbalance(uint64 busiest_load, uint64 local_load,
                                  uint64 min_threshold) {
    if (busiest_load <= local_load) return 0;

    uint64 imbalance = busiest_load - local_load;
    if (imbalance < min_threshold) return 0;

    /* Target: pull enough to equalise = (busiest - local) / 2 */
    return imbalance / 2;
}

/* ── Mode A: Periodic balance (busy CPUs) ─────────────────── */

/**
 * __eevdf_periodic_balance - Called from task_tick.  Our rq_lock is
 *   held.  Try-locks remote CPUs to avoid deadlock.
 *
 * Tiered intervals:
 *  - If last balance found severe imbalance → use fast interval (10ms)
 *  - Otherwise → normal interval (100ms)
 *  - On consecutive failures → exponential backoff (up to 8× normal)
 */
static void __eevdf_periodic_balance(struct eevdf_rq *this_erq, int this_cpu,
                                     int cls_id) {
    uint64 now = r_time();

    /* Update our EWMA load average. */
    __update_load_avg(this_erq);

    /* Determine balance interval based on state. */
    uint64 interval = EEVDF_BALANCE_INTERVAL_NORMAL;

    /* Severe imbalance detected last time → use fast interval.
     * Use PELT-based cpu_load to detect when we are lightly loaded. */
    if (this_erq->cpu_load < EEVDF_IMBALANCE_SEVERE) {
        /* We are lightly loaded — might be severely imbalanced. */
        interval = EEVDF_BALANCE_INTERVAL_FAST;
    }

    /* Exponential backoff on consecutive failures (cap at 8×). */
    uint32 failures = this_erq->balance_failures;
    if (failures > 0) {
        uint32 backoff = 1U << (failures < 3 ? failures : 3);
        interval = interval * backoff;
    }

    if (now - this_erq->last_balance_tick < interval) {
        return;
    }
    this_erq->last_balance_tick = now;
    this_erq->nr_balance++;

    if (__eevdf_rqs[cls_id] == NULL) return;

    /* Find the busiest CPU. */
    uint64 busiest_load = 0;
    int busiest_cpu = __find_busiest_cpu(this_cpu, cls_id, &busiest_load);
    if (busiest_cpu < 0) {
        this_erq->balance_failures++;
        return;
    }

    /* Compute effective local load using PELT cpu_load (weighted util).
     * Fall back to load_sum for responsiveness. */
    uint64 local_load = this_erq->cpu_load;
    if (this_erq->load_sum > local_load) local_load = this_erq->load_sum;

    uint64 target = __compute_imbalance(busiest_load, local_load,
                                        EEVDF_IMBALANCE_MIN);
    if (target == 0) {
        this_erq->balance_failures++;
        return;
    }

    /* Try to lock the remote CPU's run queue (non-blocking). */
    if (!rq_trylock(busiest_cpu)) {
        this_erq->balance_failures++;
        return;
    }

    /* Re-validate loads under lock using PELT cpu_load. */
    struct eevdf_rq *busiest_erq = &__eevdf_rqs[cls_id][busiest_cpu];
    __update_load_avg(busiest_erq);
    uint64 actual_busiest = busiest_erq->cpu_load;
    if (busiest_erq->load_sum > actual_busiest)
        actual_busiest = busiest_erq->load_sum;

    target = __compute_imbalance(actual_busiest, this_erq->cpu_load,
                                 EEVDF_IMBALANCE_MIN);
    if (target == 0) {
        rq_unlock(busiest_cpu);
        this_erq->balance_failures++;
        return;
    }

    /* Pick migratable entities. */
    struct sched_entity *candidates[EEVDF_MAX_PULL_PER_BALANCE];
    int n = __pick_migrate_entities(busiest_erq, this_cpu, target,
                                    EEVDF_MAX_PULL_PER_BALANCE, candidates);

    if (n == 0) {
        rq_unlock(busiest_cpu);
        this_erq->balance_failures++;
        return;
    }

    /* Migrate selected entities. */
    for (int i = 0; i < n; i++) {
        __do_migrate(busiest_erq, this_erq, candidates[i]);
        this_erq->nr_balance_pulled++;
    }

    rq_unlock(busiest_cpu);

    /* Reset failure counter on success. */
    this_erq->balance_failures = 0;

    /* Kick the remote CPU so it can pick a new next task. */
    ipi_send_single(busiest_cpu, IPI_REASON_RESCHEDULE);

    /* Request reschedule on this CPU. */
    SET_NEEDS_RESCHED();
}

/* ── Mode B: Idle balance (idle CPUs) ─────────────────────── */

/**
 * __eevdf_idle_balance - Called from pick_next_task when the local
 *   EEVDF rq is empty.  Aggressively tries to pull work from busy
 *   CPUs to avoid going idle when there is work to be done.
 *
 * Key differences from periodic balance:
 *  - Much lower threshold (any entity is worth pulling)
 *  - Tries multiple remote CPUs if try-lock fails
 *  - Scans by load_sum (instantaneous) for responsiveness
 *  - Returns 1 if work was pulled, 0 if staying idle
 *
 * Our rq_lock is held (this CPU).
 */
static int __eevdf_idle_balance(struct eevdf_rq *this_erq, int this_cpu,
                                int cls_id) {
    uint64 now = r_time();

    /* Cooldown: don't spam idle balance. */
    if (now - this_erq->last_idle_pull_stamp < EEVDF_IDLE_COOLDOWN) {
        return 0;
    }
    this_erq->last_idle_pull_stamp = now;
    this_erq->nr_idle_balance++;

    if (__eevdf_rqs[cls_id] == NULL) return 0;

    /* Build sorted list of candidate CPUs by load (descending).
     * We only track the top EEVDF_IDLE_MAX_TRYLOCK candidates. */
    struct {
        int cpu;
        uint64 load;
    } candidates[EEVDF_IDLE_MAX_TRYLOCK];
    int ncand = 0;

    for (int cpu = 0; cpu < cpu_possible_count(); cpu++) {
        if (cpu == this_cpu) continue;
        struct eevdf_rq *remote = &__eevdf_rqs[cls_id][cpu];
        /* Use PELT cpu_load for idle balance candidate selection. */
        uint64 rload = smp_load_acquire(&remote->cpu_load);
        uint64 rload_sum = smp_load_acquire(&remote->load_sum);
        if (rload_sum > rload) rload = rload_sum;
        uint32 rnr = smp_load_acquire(&remote->nr_running);

        /* Must have at least 2 entities (1 running + 1 migratable). */
        if (rnr < 2 && rload <= 0) continue;

        /* Insert into sorted candidates list. */
        int pos = ncand;
        if (ncand < EEVDF_IDLE_MAX_TRYLOCK) ncand++;
        /* Bubble-sort insert (small array, < 4 elements). */
        for (int j = pos; j > 0; j--) {
            if (rload > candidates[j - 1].load) {
                if (j < EEVDF_IDLE_MAX_TRYLOCK)
                    candidates[j] = candidates[j - 1];
                candidates[j - 1].cpu = cpu;
                candidates[j - 1].load = rload;
                pos = -1; /* Mark as inserted */
                break;
            }
        }
        if (pos >= 0 && pos < EEVDF_IDLE_MAX_TRYLOCK) {
            candidates[pos].cpu = cpu;
            candidates[pos].load = rload;
        }
    }

    /* Try each candidate in load-descending order. */
    for (int c = 0; c < ncand; c++) {
        int remote_cpu = candidates[c].cpu;

        /* Try-lock the remote CPU. */
        if (!rq_trylock(remote_cpu)) continue;

        struct eevdf_rq *remote_erq = &__eevdf_rqs[cls_id][remote_cpu];

        /* Re-check: remote must still have pullable entities. */
        if (remote_erq->nr_running < 1) {
            rq_unlock(remote_cpu);
            continue;
        }

        /* Pick one entity to pull (we just need one to stop being idle). */
        struct sched_entity *pull_candidates[1];
        int n = __pick_migrate_entities(remote_erq, this_cpu,
                                        (uint64)-1, /* any weight */
                                        1, pull_candidates);
        if (n == 0) {
            rq_unlock(remote_cpu);
            continue;
        }

        /* Migrate it. */
        __do_migrate(remote_erq, this_erq, pull_candidates[0]);
        this_erq->nr_idle_pulled++;

        rq_unlock(remote_cpu);

        /* Kick the remote CPU. */
        ipi_send_single(remote_cpu, IPI_REASON_RESCHEDULE);

        return 1; /* Success — we have work now. */
    }

    return 0; /* No work found. */
}

/*
 * Select target CPU's rq for task placement.
 *
 * Strategy (in priority order):
 *  1. If an allowed CPU is idle, pick it immediately (spread tasks).
 *  2. Prefer current CPU if its load is at or below average (locality bias).
 *  3. Otherwise pick the CPU with the lowest weighted load_sum.
 *
 * Reads of remote load_sum are racy (no remote locks held) but acceptable
 * for a placement heuristic — the periodic load balancer corrects any
 * poor decisions post-hoc.
 */
static struct rq *__eevdf_select_task_rq(struct rq *prev_rq,
                                         struct sched_entity *se,
                                         cpumask_t cpumask) {
    int major = MAJOR_PRIORITY(se->priority);

    if (cpumask == 0) {
        cpumask = cpu_possible_mask();
    }
    if (__eevdf_rqs[major] == NULL) {
        return ERR_PTR(-EINVAL);
    }

    int cur_cpu = cpuid();
    struct rq *best_rq = NULL;
    uint64 best_load = (uint64)-1;
    uint64 total_load = 0;
    int active_cpus = 0;

    /* First pass: find idle CPUs and compute total load for averaging.
     * Uses PELT-based cpu_load for utilization-aware placement. */
    for (int cpu = 0; cpu < cpu_possible_count(); cpu++) {
        if (!(cpumask & (1ULL << cpu))) continue;
        struct eevdf_rq *erq = &__eevdf_rqs[major][cpu];
        uint64 load = smp_load_acquire(&erq->cpu_load);
        total_load += load;
        active_cpus++;

        /* Idle CPU — pick immediately. Prefer current CPU among idles. */
        if (erq->nr_running == 0) {
            if (cpu == cur_cpu) return &erq->rq;
            if (best_rq == NULL) {
                best_rq = &erq->rq;
                best_load = 0;
            }
        }
    }

    /* If we found an idle CPU, return it. */
    if (best_load == 0 && best_rq != NULL) {
        return best_rq;
    }

    /* Compute average load for locality bias. */
    uint64 avg_load = active_cpus > 0 ? total_load / active_cpus : 0;

    /* Locality bias: prefer current CPU if its load <= average. */
    if (cpumask & (1ULL << cur_cpu)) {
        struct eevdf_rq *erq = &__eevdf_rqs[major][cur_cpu];
        uint64 cur_load = smp_load_acquire(&erq->cpu_load);
        if (cur_load <= avg_load) {
            return &erq->rq;
        }
        best_rq = &erq->rq;
        best_load = cur_load;
    }

    /* Second pass: pick least-loaded CPU by PELT cpu_load. */
    for (int cpu = 0; cpu < cpu_possible_count(); cpu++) {
        if (cpu == cur_cpu) continue;
        if (!(cpumask & (1ULL << cpu))) continue;
        struct eevdf_rq *erq = &__eevdf_rqs[major][cpu];
        uint64 load = smp_load_acquire(&erq->cpu_load);
        if (load < best_load) {
            best_rq = &erq->rq;
            best_load = load;
        }
    }

    if (best_rq == NULL) {
        return ERR_PTR(-ENOENT);
    }
    return best_rq;
}

/* ──────────────────────────────────────────────────────────────
 *  sched_class definition
 * ────────────────────────────────────────────────────────────── */
static struct sched_class __eevdf_sched_class = {
    .enqueue_task   = __eevdf_enqueue_task,
    .dequeue_task   = __eevdf_dequeue_task,
    .select_task_rq = __eevdf_select_task_rq,
    .pick_next_task = __eevdf_pick_next_task,
    .put_prev_task  = __eevdf_put_prev_task,
    .set_next_task  = __eevdf_set_next_task,
    .task_tick      = __eevdf_task_tick,
    .task_fork      = __eevdf_task_fork,
    .task_dead      = __eevdf_task_dead,
    .yield_task     = __eevdf_yield_task,
};

/* ──────────────────────────────────────────────────────────────
 *  Initialization
 * ────────────────────────────────────────────────────────────── */

static void __eevdf_rq_init(struct eevdf_rq *erq, int cls_id, int cpu_id) {
    rq_init(&erq->rq);
    rb_root_init(&erq->tasks_timeline, &__eevdf_rb_opts);
    erq->min_vruntime = 0;
    erq->nr_running = 0;

    /* Load tracking */
    erq->load_sum = 0;
    erq->load_avg = 0;
    erq->load_avg_stamp = 0;

    /* Per-CPU PELT aggregates */
    erq->cpu_util = 0;
    erq->cpu_load = 0;

    /* Periodic balance state */
    erq->last_balance_tick = 0;
    erq->last_balance_failed = 0;
    erq->balance_failures = 0;
    erq->nr_balance = 0;
    erq->nr_balance_pulled = 0;

    /* Idle balance state */
    erq->last_idle_pull_stamp = 0;
    erq->nr_idle_balance = 0;
    erq->nr_idle_pulled = 0;

    rq_clear_ready(cls_id, cpu_id);
}

static void __alloc_eevdf_rqs_for_cls(int cls_id) {
    size_t sz = sizeof(struct eevdf_rq) * cpu_possible_count();
    __eevdf_rqs[cls_id] = (struct eevdf_rq *)kvmalloc(sz);
    if (!__eevdf_rqs[cls_id]) {
        panic("alloc_eevdf_rqs: failed to allocate for cls_id %d\n", cls_id);
    }
    memset(__eevdf_rqs[cls_id], 0, sz);
    for (int i = 0; i < cpu_possible_count(); i++) {
        __eevdf_rq_init(&__eevdf_rqs[cls_id][i], cls_id, i);
        rq_register(&__eevdf_rqs[cls_id][i].rq, cls_id, i);
    }
}

void init_eevdf_rq(void) {
    for (int cls_id = EEVDF_MAJOR_PRIORITY_START;
         cls_id < EEVDF_MAJOR_PRIORITY_LIMIT; cls_id++) {
        sched_class_register(cls_id, &__eevdf_sched_class);
        __alloc_eevdf_rqs_for_cls(cls_id);
    }
}
