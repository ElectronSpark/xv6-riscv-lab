// Memory watermarks, kswapd, and direct reclaim
//
// This module implements the core memory reclamation infrastructure,
// modeled after Linux's memory management:
//
// 1. WATERMARKS: Three thresholds (MIN/LOW/HIGH) computed as percentages
//    of total memory. These drive reclamation decisions.
//
// 2. KSWAPD: A background kernel thread that wakes when free pages drop
//    below WMARK_LOW. It reclaims pages via the shrinker framework until
//    free pages reach WMARK_HIGH, then goes back to sleep.
//
// 3. DIRECT RECLAIM: When an allocation finds free pages below WMARK_MIN,
//    the allocating thread itself performs synchronous reclamation before
//    retrying the allocation.
//
// 4. OOM INTEGRATION: If all reclamation attempts fail and free pages
//    remain below WMARK_MIN, the OOM killer is invoked.

#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "printf.h"
#include "defs.h"
#include "string.h"
#include <mm/page.h>
#include <mm/mm_watermark.h>
#include <mm/shrinker.h>
#include <mm/oom_kill.h>
#include <mm/slab.h>
#include <mm/pcache.h>
#include "proc/thread.h"
#include "proc/sched.h"
#include "smp/atomic.h"
#include "timer/timer.h"

// ============================================================================
// Global State
// ============================================================================

static struct mm_watermark_state __wmark_state = {0};
static spinlock_t __wmark_lock = SPINLOCK_INITIALIZED("wmark_lock");

// kswapd synchronization
static struct thread *__kswapd_thread = NULL;
static _Atomic int __kswapd_should_wake = 0;
static _Atomic int __kswapd_running = 0;

// Reclaim statistics
static _Atomic uint64 __reclaim_direct_count = 0;
static _Atomic uint64 __reclaim_kswapd_count = 0;

// ============================================================================
// Watermark Computation
// ============================================================================

static uint64 __get_free_pages_fast(void) {
    return get_total_free_pages();
}

// Compute watermark thresholds based on total managed pages
static void __compute_watermarks(uint64 total_pages) {
    uint64 wmin = (total_pages * WMARK_MIN_PERCENT) / 100;
    uint64 wlow = (total_pages * WMARK_LOW_PERCENT) / 100;
    uint64 whigh = (total_pages * WMARK_HIGH_PERCENT) / 100;

    // Enforce minimums
    if (wmin < WMARK_MIN_PAGES) wmin = WMARK_MIN_PAGES;
    if (wlow < WMARK_LOW_PAGES) wlow = WMARK_LOW_PAGES;
    if (whigh < WMARK_HIGH_PAGES) whigh = WMARK_HIGH_PAGES;

    // Ensure ordering: min < low < high
    if (wlow <= wmin) wlow = wmin + 16;
    if (whigh <= wlow) whigh = wlow + 16;

    __wmark_state.wmark[WMARK_MIN] = wmin;
    __wmark_state.wmark[WMARK_LOW] = wlow;
    __wmark_state.wmark[WMARK_HIGH] = whigh;
    __wmark_state.total_pages = total_pages;
}

void mm_watermark_init(void) {
    uint64 total = __get_free_pages_fast();
    // At init time, nearly all pages are free, so use that as total
    __compute_watermarks(total);

    __atomic_store_n(&__wmark_state.pressure_level, MEM_PRESSURE_NONE,
                     __ATOMIC_RELEASE);

    printf("Memory watermarks: min=%ld low=%ld high=%ld (total=%ld pages, %ldMB)\n",
           __wmark_state.wmark[WMARK_MIN],
           __wmark_state.wmark[WMARK_LOW],
           __wmark_state.wmark[WMARK_HIGH],
           total, (total * PAGE_SIZE) / (1024 * 1024));
}

void mm_watermark_recompute(void) {
    spin_lock(&__wmark_lock);
    uint64 total = __wmark_state.total_pages;
    if (total == 0)
        total = __get_free_pages_fast();
    __compute_watermarks(total);
    spin_unlock(&__wmark_lock);
}

// ============================================================================
// Watermark Checks
// ============================================================================

bool mm_watermark_ok(enum wmark_level level) {
    if (level >= NR_WMARK)
        return false;
    uint64 free = __get_free_pages_fast();
    return free >= __wmark_state.wmark[level];
}

enum mem_pressure mm_get_pressure(void) {
    uint64 free = __get_free_pages_fast();

    if (free >= __wmark_state.wmark[WMARK_HIGH])
        return MEM_PRESSURE_NONE;
    if (free >= __wmark_state.wmark[WMARK_LOW])
        return MEM_PRESSURE_LOW;
    if (free >= __wmark_state.wmark[WMARK_MIN])
        return MEM_PRESSURE_MEDIUM;
    return MEM_PRESSURE_CRITICAL;
}

const struct mm_watermark_state *mm_watermark_get_state(void) {
    return &__wmark_state;
}

// ============================================================================
// Direct Reclaim
// ============================================================================

// Attempt to reclaim memory directly (synchronous, in allocator context).
// Tries shrinkers first, then pcache eviction, then slab shrinking.
// Returns pages freed.
static uint64 __direct_reclaim(uint64 order) {
    uint64 needed = 1ULL << order;
    uint64 total_freed = 0;

    __atomic_fetch_add(&__reclaim_direct_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&__wmark_state.direct_reclaim_count, 1,
                       __ATOMIC_RELAXED);

    // Phase 1: Ask shrinkers to free pages proportionally
    uint64 freed = shrink_all_caches(needed * 2);
    total_freed += freed;
    if (__get_free_pages_fast() >= __wmark_state.wmark[WMARK_MIN] + needed)
        goto done;

    // Phase 2: Aggressively shrink pcache LRU (evict clean pages)
    pcache_shrink_caches();
    freed = shrink_all_caches(needed * 4);
    total_freed += freed;
    if (__get_free_pages_fast() >= __wmark_state.wmark[WMARK_MIN] + needed)
        goto done;

    // Phase 3: Emergency slab shrink
    kmm_shrink_all();
    slab_shrink_all();

done:
    __atomic_fetch_add(&__wmark_state.pages_reclaimed, total_freed,
                       __ATOMIC_RELAXED);
    return total_freed;
}

// Public reclaim entry point — called from page allocator
uint64 mm_try_reclaim(uint64 order, uint64 gfp_flags) {
    // Don't recurse if we're already in reclaim
    if (gfp_flags & GFP_NORECLAIM)
        return 0;

    uint64 freed = __direct_reclaim(order);

    // If direct reclaim wasn't enough, invoke OOM killer
    uint64 needed = 1ULL << order;
    if (__get_free_pages_fast() < __wmark_state.wmark[WMARK_MIN] + needed) {
        int oom_ret = oom_kill_process(order, gfp_flags);
        if (oom_ret == OOM_SUCCESS) {
            __atomic_fetch_add(&__wmark_state.oom_kill_count, 1,
                               __ATOMIC_RELAXED);
        }
        // After OOM kill, give the victim time to die and release pages
        // The caller should retry allocation after this returns
    }

    return freed;
}

// ============================================================================
// kswapd — Background Reclaim Daemon
// ============================================================================

// kswapd main loop: sleep until woken by watermark crossing, then reclaim
// until free pages reach WMARK_HIGH.
static void __kswapd_main(void) {
    __atomic_store_n(&__kswapd_running, 1, __ATOMIC_RELEASE);

    for (;;) {
        // Sleep until woken or periodic check (every 5 seconds)
        sleep_ms(5000);

        uint64 free = __get_free_pages_fast();

        if (free >= __wmark_state.wmark[WMARK_HIGH]) {
            __atomic_store_n(&__wmark_state.pressure_level,
                             MEM_PRESSURE_NONE, __ATOMIC_RELEASE);
            __atomic_store_n(&__kswapd_should_wake, 0, __ATOMIC_RELEASE);
            continue;
        }

        // We're below WMARK_HIGH — start reclaiming
        __atomic_fetch_add(&__wmark_state.kswapd_wakeup_count, 1,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&__reclaim_kswapd_count, 1, __ATOMIC_RELAXED);

        // Update pressure level
        enum mem_pressure pressure = mm_get_pressure();
        __atomic_store_n(&__wmark_state.pressure_level, pressure,
                         __ATOMIC_RELEASE);

        // Reclaim in rounds until we reach WMARK_HIGH
        int rounds = 0;
        int max_rounds = 16; // Prevent infinite loop

        while (rounds < max_rounds) {
            free = __get_free_pages_fast();
            if (free >= __wmark_state.wmark[WMARK_HIGH])
                break;

            uint64 deficit = __wmark_state.wmark[WMARK_HIGH] - free;

            // Ask shrinkers to free the deficit
            uint64 freed = shrink_all_caches(deficit);
            __atomic_fetch_add(&__wmark_state.pages_reclaimed, freed,
                               __ATOMIC_RELAXED);

            if (freed == 0) {
                // Shrinkers couldn't free anything, try harder
                pcache_shrink_caches();
                kmm_shrink_all();
                slab_shrink_all();

                // Check again after emergency shrink
                free = __get_free_pages_fast();
                if (free >= __wmark_state.wmark[WMARK_LOW])
                    break;

                // Nothing more we can do in background
                break;
            }

            rounds++;
        }

        // Update final pressure level
        pressure = mm_get_pressure();
        __atomic_store_n(&__wmark_state.pressure_level, pressure,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&__kswapd_should_wake, 0, __ATOMIC_RELEASE);
    }
}

void kswapd_start(void) {
    __kswapd_thread = kthread_create("kswapd", (void *)__kswapd_main, 0, 0, 0);
    if (__kswapd_thread == NULL) {
        printf("WARNING: failed to create kswapd thread\n");
        return;
    }
    printf("kswapd started (tid=%d)\n", __kswapd_thread->pid);
}

void kswapd_wake(void) {
    if (__kswapd_thread != NULL &&
        !__atomic_load_n(&__kswapd_should_wake, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&__kswapd_should_wake, 1, __ATOMIC_RELEASE);
        wakeup(__kswapd_thread);
    }
}

// ============================================================================
// Diagnostics
// ============================================================================

void mm_watermark_dump(void) {
    uint64 free = __get_free_pages_fast();
    enum mem_pressure p = mm_get_pressure();

    static const char *pressure_names[] = {
        "none", "low", "medium", "critical", "OOM"
    };

    printf("=== Memory Watermark Status ===\n");
    printf("  Total pages:     %ld (%ldMB)\n",
           __wmark_state.total_pages,
           (__wmark_state.total_pages * PAGE_SIZE) / (1024 * 1024));
    printf("  Free pages:      %ld (%ldMB)\n",
           free, (free * PAGE_SIZE) / (1024 * 1024));
    printf("  WMARK_MIN:       %ld pages\n", __wmark_state.wmark[WMARK_MIN]);
    printf("  WMARK_LOW:       %ld pages\n", __wmark_state.wmark[WMARK_LOW]);
    printf("  WMARK_HIGH:      %ld pages\n", __wmark_state.wmark[WMARK_HIGH]);
    printf("  Pressure:        %s\n",
           p < sizeof(pressure_names)/sizeof(pressure_names[0])
               ? pressure_names[p] : "unknown");
    printf("  Direct reclaims: %ld\n",
           __atomic_load_n(&__wmark_state.direct_reclaim_count,
                           __ATOMIC_RELAXED));
    printf("  kswapd wakeups:  %ld\n",
           __atomic_load_n(&__wmark_state.kswapd_wakeup_count,
                           __ATOMIC_RELAXED));
    printf("  Pages reclaimed: %ld\n",
           __atomic_load_n(&__wmark_state.pages_reclaimed,
                           __ATOMIC_RELAXED));
    printf("  OOM kills:       %ld\n",
           __atomic_load_n(&__wmark_state.oom_kill_count,
                           __ATOMIC_RELAXED));
    printf("===============================\n");
}
