#ifndef __KERNEL_SLAB_H
#define __KERNEL_SLAB_H

#include "compiler.h"
#include "slab_type.h"

#define SLAB_CACHE_NUMS             8
#define SLAB_OBJ_MAX_SIZE           PAGE_SIZE
#define SLAB_OBJ_MIN_SIZE           32
// Try to set the default SLAB size as 2MB, which is the size of a huge page.
#if PAGE_BUDDY_MAX_ORDER > 8
#define SLAB_DEFAULT_ORDER          8
#else
#define SLAB_DEFAULT_ORDER          PAGE_BUDDY_MAX_ORDER
#endif

int slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, 
                    uint64 flags);
slab_cache_t *slab_cache_create(char *name, size_t obj_size, uint64 flags);
int slab_cache_destroy(slab_cache_t *cache);
int slab_cache_shrink(slab_cache_t *cache, int nums);
void *slab_alloc(slab_cache_t *cache);
void slab_free(void *obj);

#endif          /* __KERNEL_SLAB_H */
