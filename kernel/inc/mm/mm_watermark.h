// Memory watermarks — zone-based memory pressure thresholds
//
// Modeled after Linux's zone watermarks (WMARK_MIN, WMARK_LOW, WMARK_HIGH).
// The watermark system drives memory reclamation policy:
//
//   free >= WMARK_HIGH  — system healthy, no reclamation
//   free >= WMARK_LOW   — wake background kswapd to start reclaiming
//   free >= WMARK_MIN   — direct reclaim in allocator context
//   free <  WMARK_MIN   — emergency: only essential (GFP_ATOMIC) allocations
//
// Watermarks are computed as fractions of total managed pages and can be
// tuned at runtime.
#ifndef __KERNEL_MM_WATERMARK_H
#define __KERNEL_MM_WATERMARK_H

#include "types.h"

// Watermark levels
enum wmark_level {
    WMARK_MIN = 0,   // Absolute minimum — direct reclaim required
    WMARK_LOW,       // Background reclaim threshold (wake kswapd)
    WMARK_HIGH,      // System healthy — no reclamation needed
    NR_WMARK
};

// Memory pressure levels (for notifications)
enum mem_pressure {
    MEM_PRESSURE_NONE = 0, // Free pages >= WMARK_HIGH
    MEM_PRESSURE_LOW,      // Free pages < WMARK_HIGH (kswapd woken)
    MEM_PRESSURE_MEDIUM,   // Free pages < WMARK_LOW (direct reclaim active)
    MEM_PRESSURE_CRITICAL, // Free pages < WMARK_MIN (OOM imminent)
    MEM_PRESSURE_OOM,      // Out of memory — OOM killer invoked
};

// Watermark state — global memory statistics
struct mm_watermark_state {
    // Watermark thresholds (in pages)
    uint64 wmark[NR_WMARK];

    // Current memory statistics (updated on check)
    uint64 total_pages;       // Total managed pages
    uint64 free_pages;        // Current free pages
    uint64 cached_pages;      // Pages in per-CPU caches

    // Pressure tracking
    _Atomic int pressure_level;  // Current enum mem_pressure
    _Atomic uint64 direct_reclaim_count;   // Times direct reclaim was invoked
    _Atomic uint64 kswapd_wakeup_count;    // Times kswapd was woken
    _Atomic uint64 pages_reclaimed;        // Total pages reclaimed
    _Atomic uint64 oom_kill_count;         // OOM kills performed
};

// Default watermark percentages of total memory
// These match Linux's default behavior roughly:
//   min = ~0.5% of total, low = ~1%, high = ~1.5%
// Tuned for xv6's smaller memory footprint
#define WMARK_MIN_PERCENT  1   // 1% of total
#define WMARK_LOW_PERCENT  2   // 2% of total
#define WMARK_HIGH_PERCENT 3   // 3% of total

// Minimum watermark values (in pages) to avoid zero watermarks on small systems
#define WMARK_MIN_PAGES  32
#define WMARK_LOW_PAGES  64
#define WMARK_HIGH_PAGES 128

// GFP flags for allocation behavior under pressure
#define GFP_NORECLAIM  (1ULL << 33) // Do not invoke reclaim (for reclaim path itself)
#define GFP_NOWATERMARK (1ULL << 34) // Skip watermark check (emergency/reclaim)

// Initialize watermark system and compute initial thresholds
void mm_watermark_init(void);

// Recompute watermarks (call after memory hotplug or configuration change)
void mm_watermark_recompute(void);

// Check current free pages against watermarks, return true if above @level
bool mm_watermark_ok(enum wmark_level level);

// Get current pressure level
enum mem_pressure mm_get_pressure(void);

// Get global watermark state (for diagnostics)
const struct mm_watermark_state *mm_watermark_get_state(void);

// Try to reclaim pages to satisfy allocation of @order pages
// Returns number of pages reclaimed
uint64 mm_try_reclaim(uint64 order, uint64 gfp_flags);

// Start the kswapd background reclaim thread
void kswapd_start(void);

// Wake kswapd if sleeping (called when watermark crossed)
void kswapd_wake(void);

// Print watermark statistics
void mm_watermark_dump(void);

#endif /* __KERNEL_MM_WATERMARK_H */
