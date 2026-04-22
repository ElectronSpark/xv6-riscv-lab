// Shrinker framework — generic cache shrinking interface
//
// Modeled after Linux's struct shrinker. Subsystems that maintain
// reclaimable caches (slab, pcache, dentry cache, etc.) register a
// shrinker so the memory reclaim path can ask them to release pages
// under memory pressure.
//
// Each shrinker provides two callbacks:
//   count_objects — return the number of reclaimable objects
//   scan_objects  — free up to @nr_to_scan objects, return freed count
//
// The reclaim path iterates all registered shrinkers proportionally.
#ifndef __KERNEL_SHRINKER_H
#define __KERNEL_SHRINKER_H

#include "types.h"
#include "list_type.h"

struct shrinker;

// Shrink control — passed to scan_objects to describe reclaim request
struct shrink_control {
    uint64 nr_to_scan;   // Number of objects to try to free
    uint64 nr_scanned;   // Objects actually scanned (set by callback)
};

// Shrinker callbacks
struct shrinker {
    // Return number of freeable objects, or 0 if nothing reclaimable.
    uint64 (*count_objects)(struct shrinker *s, struct shrink_control *sc);

    // Scan and free up to sc->nr_to_scan objects.
    // Return number of objects freed, or SHRINK_STOP if cannot free more.
    uint64 (*scan_objects)(struct shrinker *s, struct shrink_control *sc);

    int seeks;             // Cost of recreating an object (higher = avoid shrinking)
    void *private_data;    // Opaque pointer for the registering subsystem
    list_node_t list;      // Link in global shrinker list
    const char *name;      // Debug name
};

#define SHRINK_STOP (~0ULL) // Return from scan_objects to indicate stop

#define DEFAULT_SEEKS 2
#define SHRINKER_INIT(nm, cnt, scan, sk) { \
    .count_objects = (cnt),                \
    .scan_objects = (scan),                \
    .seeks = (sk),                         \
    .private_data = NULL,                  \
    .name = (nm),                          \
}

// Register/unregister a shrinker
void shrinker_register(struct shrinker *s);
void shrinker_unregister(struct shrinker *s);

// Shrink all registered caches, freeing up to @nr_pages pages worth of objects.
// Returns total pages freed.
uint64 shrink_all_caches(uint64 nr_pages);

// Initialize the shrinker framework
void shrinker_init(void);

#endif /* __KERNEL_SHRINKER_H */
