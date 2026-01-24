#ifndef __KERNEL_SLAB_H
#define __KERNEL_SLAB_H

#include "compiler.h"
#include "slab_type.h"

#define SLAB_CACHE_NUMS             8
#define SLAB_OBJ_MAX_SHIFT          PAGE_SHIFT
#define SLAB_OBJ_MAX_SIZE           PAGE_SIZE
#define SLAB_OBJ_MIN_SHIFT          5
#define SLAB_OBJ_MIN_SIZE           (1UL << SLAB_OBJ_MIN_SHIFT)
// DEPRECATED: SLAB_DEFAULT_ORDER is no longer used.
// Slab order is now determined adaptively based on object size in slab_cache_init().
// This macro is kept for compatibility but has no effect.
#if PAGE_BUDDY_MAX_ORDER > 8
#define SLAB_DEFAULT_ORDER          8
#else
#define SLAB_DEFAULT_ORDER          PAGE_BUDDY_MAX_ORDER
#endif

#define ITABLE_INODE_HASH_BUCKETS  31

int slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, 
                    uint64 flags);
slab_cache_t *slab_cache_create(char *name, size_t obj_size, uint64 flags);
int slab_cache_destroy(slab_cache_t *cache);
int slab_cache_shrink(slab_cache_t *cache, int nums);
void slab_shrink_all(void);  // Shrink all registered slab caches (for OOM recovery)
uint64 slab_dump_all(int detailed);  // Dump statistics for all slab caches, return total bytes
void *slab_alloc(slab_cache_t *cache);
void slab_free(void *obj);
void slab_free_noshrink(void *obj);

#endif          /* __KERNEL_SLAB_H */
