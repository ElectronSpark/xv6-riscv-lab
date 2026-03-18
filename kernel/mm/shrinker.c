// Shrinker framework — generic cache shrinking for memory reclamation
//
// All reclaimable kernel caches register a shrinker. The reclaim path
// iterates the global list and asks each to free objects proportionally.
// Modeled after Linux's shrinker infrastructure.

#include "types.h"
#include "lock/spinlock.h"
#include "list.h"
#include "printf.h"
#include "defs.h"
#include <mm/shrinker.h>

// Global shrinker list
static list_node_t __shrinker_list = LIST_ENTRY_INITIALIZED(__shrinker_list);
static spinlock_t __shrinker_lock = SPINLOCK_INITIALIZED("shrinker_lock");
static int __shrinker_count = 0;

void shrinker_init(void) {
    // Already statically initialized
    printf("Shrinker framework initialized\n");
}

void shrinker_register(struct shrinker *s) {
    if (s == NULL)
        return;
    list_entry_init(&s->list);
    spin_lock(&__shrinker_lock);
    list_entry_push_back(&__shrinker_list, &s->list);
    __shrinker_count++;
    spin_unlock(&__shrinker_lock);
}

void shrinker_unregister(struct shrinker *s) {
    if (s == NULL)
        return;
    spin_lock(&__shrinker_lock);
    list_entry_detach(&s->list);
    __shrinker_count--;
    spin_unlock(&__shrinker_lock);
}

// Max shrinkers we snapshot. Sufficient for all kernel caches.
#define MAX_SHRINKERS 16

// Shrink all registered caches, attempting to free @nr_pages pages.
// Uses proportional scanning: each shrinker is asked to free a share
// proportional to its reclaimable object count.
//
// We snapshot shrinker pointers under the lock, then invoke callbacks
// without holding it. This avoids:
//   1. Infinite loop if a shrinker is unregistered while iterating
//      (detached list nodes self-point, causing LIST_NEXT_NODE to loop)
//   2. Lock ordering issues (callbacks take subsystem locks)
uint64 shrink_all_caches(uint64 nr_pages) {
    uint64 total_freed = 0;
    uint64 total_objects = 0;

    if (nr_pages == 0)
        return 0;

    // Snapshot shrinker pointers while holding the lock
    struct shrinker *snap[MAX_SHRINKERS];
    int n = 0;
    struct shrinker *s, *__tmp;

    spin_lock(&__shrinker_lock);
    list_foreach_node_safe(&__shrinker_list, s, __tmp, list) {
        if (n < MAX_SHRINKERS)
            snap[n++] = s;
    }
    spin_unlock(&__shrinker_lock);

    if (n == 0)
        return 0;

    // First pass (lock-free): count total reclaimable objects
    uint64 counts[MAX_SHRINKERS];
    for (int i = 0; i < n; i++) {
        struct shrink_control sc = {0};
        counts[i] = snap[i]->count_objects(snap[i], &sc);
        if (counts[i] != SHRINK_STOP)
            total_objects += counts[i];
        else
            counts[i] = 0;
    }

    if (total_objects == 0)
        return 0;

    // Second pass (lock-free): scan proportionally
    for (int i = 0; i < n; i++) {
        if (counts[i] == 0)
            continue;

        // Proportional: this shrinker's share of nr_pages
        uint64 scan = (nr_pages * counts[i]) / total_objects;
        if (scan == 0)
            scan = 1;

        // Scale by seeks cost: higher seeks = scan fewer objects
        if (snap[i]->seeks > 1)
            scan = (scan + snap[i]->seeks - 1) / snap[i]->seeks;
        if (scan == 0)
            scan = 1;

        struct shrink_control sc = {0};
        sc.nr_to_scan = scan;

        uint64 freed = snap[i]->scan_objects(snap[i], &sc);
        if (freed != SHRINK_STOP)
            total_freed += freed;

        if (total_freed >= nr_pages)
            break;
    }

    return total_freed;
}
