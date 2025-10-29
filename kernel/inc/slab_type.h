#ifndef __KERNEL_SLAB_TYPE_H
#define __KERNEL_SLAB_TYPE_H

#include "types.h"
#include "param.h"
#include "list_type.h"
#include "spinlock.h"


typedef struct page_struct page_t;
typedef struct slab_struct slab_t;

typedef struct slab_cpu_state_struct {
    spinlock_t              lock;
    list_node_t             partial_list;
    int64                   slab_partial;
} slab_cpu_state_t;

typedef struct slab_cache_struct {
    const char              *name;
    uint64                  flags;
#define SLAB_FLAG_STATIC            1UL
#define SLAB_FLAG_EMBEDDED          2UL
    // The size of each object in this SLAB cache
    size_t                  obj_size;
    // If SLAB descriptor is embedded in the page storing objects, then objects
    // start from an offset of the page
    size_t                  offset;
    // Each SLAB has 2**cache_order pages
    uint32                  slab_order;
    // The number of objects in each slab
    uint32                  slab_obj_num;
    // When the number of free objects reachs limits, the slab will try to
    // free half of its SLABs.
    uint32                  limits;

    // list head linking slab_t (partial lists are per-CPU)
    list_node_t             free_list;
    slab_cpu_state_t        cpu_state[NCPU];
    list_node_t             full_list;

    // Count the number of cache
    int64                  slab_free;
    int64                  slab_partial_total;
    int64                  slab_full;
    int64                  slab_total;

    // count the objects
    uint64                  obj_active;
    uint64                  obj_total;

    // locks protecting global resources
    spinlock_t              global_lock;
} slab_cache_t;

typedef struct slab_struct {
    spinlock_t              lock;
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
    // owner cpu for per-cpu partial list; -1 if not assigned
    int                     owner_cpu;
} slab_t;

#endif          /* __KERNEL_SLAB_TYPE_H */
