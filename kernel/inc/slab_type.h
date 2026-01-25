#ifndef __KERNEL_SLAB_TYPE_H
#define __KERNEL_SLAB_TYPE_H

#include "types.h"
#include "param.h"
#include "list_type.h"
#include "lock/spinlock.h"


typedef struct page_struct page_t;
typedef struct slab_struct slab_t;

// Per-CPU slab cache structure
typedef struct {
    list_node_t     partial_list;       // Per-CPU partial slabs
    list_node_t     full_list;          // Per-CPU full slabs
    _Atomic uint32  partial_count;      // Number of partial slabs (atomic)
    _Atomic uint32  full_count;         // Number of full slabs (atomic)
    spinlock_t      lock;               // Protects this CPU's lists
} percpu_slab_cache_t;

typedef struct slab_cache_struct {
    const char              *name;
    uint64                  flags;
#define SLAB_FLAG_STATIC            1UL
#define SLAB_FLAG_EMBEDDED          2UL
#define SLAB_FLAG_DEBUG_BITMAP      4UL  // Enable bitmap tracking for debugging
    // The size of each object in this SLAB cache
    size_t                  obj_size;
    // If SLAB descriptor is embedded in the page storing objects, then objects
    // start from an offset of the page
    size_t                  offset;
    // Each SLAB has 2**cache_order pages
    uint32                  slab_order;
    // The number of objects in each slab
    uint32                  slab_obj_num;
    // Size of the bitmap in uint64 words (0 if bitmap disabled)
    uint32                  bitmap_size;
    // When the number of free objects reachs limits, the slab will try to
    // free half of its SLABs.
    uint32                  limits;

    // Per-CPU caches (one per CPU)
    percpu_slab_cache_t     percpu_caches[NCPU];

    // Global free list (shared across all CPUs)
    list_node_t             global_free_list;
    spinlock_t              global_free_lock;
    _Atomic int64           global_free_count;

    // Global counters (atomic for lock-free reads)
    _Atomic int64           slab_total;
    _Atomic uint64          obj_active;
    _Atomic uint64          obj_total;

    // Link to global list of all slab caches (for shrinking)
    list_node_t             cache_list_entry;
} slab_cache_t;

typedef enum {
    SLAB_STATE_DEQUEUED = 0,
    SLAB_STATE_FREE,
    SLAB_STATE_PARTIAL,
    SLAB_STATE_FULL
} slab_state_t;

typedef struct slab_struct {
    list_node_t             list_entry;
    // pointing its slab descriptor
    slab_cache_t            *cache;
    // pointing to the page descriptor where stores its objects
    page_t                  *page;
    // Each SLAB has 2**cache_order pages
    uint16                  slab_order;
    // number of objects in use
    uint64                  in_use;
    // the next free objects
    void                    *next;
    // current state of the SLAB
    // indicates which list the SLAB is in
    slab_state_t            state;
    // optional bitmap for tracking allocation/deallocation (NULL if disabled)
    uint64                  *bitmap;
    // CPU ID that owns this slab (-1 for global free list, 0-7 for CPU ID)
    _Atomic int             cpu_id;
} slab_t;

#endif          /* __KERNEL_SLAB_TYPE_H */
